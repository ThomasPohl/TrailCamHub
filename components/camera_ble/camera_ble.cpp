/*
 * This module activates the camera's Wi-Fi over BLE with the NimBLE host:
 * scan for the camera, connect, discover its GATT profile, subscribe to the
 * response characteristic and write the Wi-Fi-enable command. The NimBLE
 * stack runs only while activation is wanted and can be released afterwards
 * to return its memory.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_hosted.h"
#endif

#include "ble_advertising.hpp"
#include "camera_ble.h"
#include "camera_ble_discovery.h"
#include "camera_ble_protocol.h"
#include "gatt_profile.hpp"

#define WIFI_CONTROL_CHAR_UUID 0x0002
#define WIFI_RESPONSE_CHAR_UUID 0x0003
#define CLIENT_CHARACTERISTIC_CONFIG_UUID 0x2902
#define SCAN_DURATION_MS 30000
#define CONNECT_TIMEOUT_MS 10000
#define MAX_BLE_DATA_LEN 31
#define MAX_BLE_DEVICE_NAME_LEN 32
#define MAX_NOTIFICATION_LEN 256

#if CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION
static const bool kReleaseAfterActivation = true;
#else
static const bool kReleaseAfterActivation = false;
#endif

using camera::ble::CameraEndpoints;
using camera::ble::GattProfile;

static const char *TAG = "trailcamhub_ble";

// Protects the state below: NimBLE callbacks run in the NimBLE host task,
// camera_ble_resume()/camera_ble_pause() in the session controller task. The
// lock is never held while the stack is started or stopped.
static SemaphoreHandle_t s_lock;
// While false, no scan is started and no camera is connected.
static bool s_activation_enabled;
// The NimBLE stack (controller and host) is initialized and running.
static bool s_stack_running;
// The host is synchronized with the controller and can scan.
static bool s_synced;
static uint8_t s_own_addr_type;
static bool s_scanning;
static bool s_connecting;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static camera_ble_link_cb_t s_link_callback;
// Last state reported to s_link_callback.
static bool s_link_active;
static char s_device_name[MAX_BLE_DEVICE_NAME_LEN];

static GattProfile s_profile;
static size_t s_discovery_service_index;
static CameraEndpoints s_endpoints;
static uint16_t s_cccd_handle;

class BleLock
{
  public:
    BleLock()
    {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
    ~BleLock()
    {
        xSemaphoreGiveRecursive(s_lock);
    }
    BleLock(const BleLock &) = delete;
    BleLock &operator=(const BleLock &) = delete;
};

static int gap_event_handler(struct ble_gap_event *event, void *arg);

// --- Logging helpers ---

static void log_hex_data(const char *label, const uint8_t *data, size_t length)
{
    char formatted[3 * MAX_BLE_DATA_LEN + 1];
    if (length == 0) {
        ESP_LOGI(TAG, "%s: empty", label);
        return;
    }
    for (size_t offset = 0; offset < length; offset += MAX_BLE_DATA_LEN) {
        size_t chunk_len = length - offset;
        size_t formatted_len = 0;
        if (chunk_len > MAX_BLE_DATA_LEN) {
            chunk_len = MAX_BLE_DATA_LEN;
        }
        for (size_t i = 0; i < chunk_len; i++) {
            int written = snprintf(formatted + formatted_len,
                                   sizeof(formatted) - formatted_len,
                                   i == 0 ? "%02X" : " %02X", data[offset + i]);
            if (written < 0 ||
                (size_t)written >= sizeof(formatted) - formatted_len) {
                return;
            }
            formatted_len += (size_t)written;
        }
        ESP_LOGI(TAG, "%s (%u bytes): %s", label, (unsigned)length, formatted);
    }
}

static uint16_t uuid16_of(const ble_uuid_any_t *uuid)
{
    return uuid->u.type == BLE_UUID_TYPE_16 ? uuid->u16.value : 0;
}

static void log_uuid(const char *kind, const ble_uuid_any_t *uuid)
{
    char text[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "Device=\"%s\": discovered GATT %s UUID=%s", s_device_name,
             kind, ble_uuid_to_str(&uuid->u, text));
}

static void copy_device_name(const uint8_t *data, size_t length)
{
    const uint8_t *name = NULL;
    size_t name_len = 0;
    if (!camera::ble::find_device_name(data, length, &name, &name_len) ||
        name_len == 0) {
        snprintf(s_device_name, sizeof(s_device_name), "unknown");
        return;
    }
    if (name_len >= sizeof(s_device_name)) {
        name_len = sizeof(s_device_name) - 1;
    }
    memcpy(s_device_name, name, name_len);
    s_device_name[name_len] = '\0';
}

// --- Scanning and connecting (called with the lock held) ---

static void start_scan(void)
{
    if (!s_activation_enabled || !s_synced || s_scanning || s_connecting ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_gap_disc_params params = {};
    params.itvl = 0x50;
    params.window = 0x30;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    params.passive = 0;
    params.filter_duplicates = 0;
    int rc = ble_gap_disc(s_own_addr_type, SCAN_DURATION_MS, &params,
                          gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start BLE scan: %d", rc);
        return;
    }
    s_scanning = true;
    ESP_LOGI(TAG, "Scanning for BLE camera %s", CAMERA_BLE_TARGET_NAME);
}

// Reports the start and the end of a camera connection, once each.
static void set_link_active(bool active)
{
    if (active == s_link_active) {
        return;
    }
    s_link_active = active;
    if (s_link_callback != NULL) {
        s_link_callback(active);
    }
}

// Called when a connection attempt or connection has ended. While the stack
// is released after the activation, the end is reported only by stop_stack(),
// once the stack's memory is free: opening the access point while the stack
// still held it failed for lack of internal RAM and crashed the Wi-Fi driver.
static void connection_ended(void)
{
    if (!kReleaseAfterActivation) {
        set_link_active(false);
    }
}

static void disconnect_camera(const char *reason)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ESP_LOGI(TAG, "%s; disconnecting BLE device", reason);
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Could not disconnect BLE device: %d", rc);
    }
}

static void handle_advertisement(const struct ble_gap_disc_desc *disc)
{
    bool connectable = disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                       disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    if (!s_activation_enabled || !connectable || s_connecting ||
        !camera_ble_is_candidate_advertisement(disc->data, disc->length_data)) {
        return;
    }

    const uint8_t *address = disc->addr.val;
    ESP_LOGI(TAG,
             "BLE device advertisement: address=%02X:%02X:%02X:%02X:%02X:%02X "
             "address_type=%u RSSI=%d dBm event_type=%u",
             address[5], address[4], address[3], address[2], address[1],
             address[0], (unsigned)disc->addr.type, disc->rssi,
             (unsigned)disc->event_type);
    log_hex_data("BLE advertising data", disc->data, disc->length_data);
    copy_device_name(disc->data, disc->length_data);

    ESP_LOGI(TAG, "Found compatible camera candidate; stopping scan before "
                  "connection");
    ble_gap_disc_cancel();
    s_scanning = false;
    set_link_active(true);
    int rc = ble_gap_connect(s_own_addr_type, &disc->addr, CONNECT_TIMEOUT_MS,
                             NULL, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not connect to BLE device: %d", rc);
        connection_ended();
        start_scan();
        return;
    }
    s_connecting = true;
}

// --- GATT client sequence: services, characteristics, CCCD, command ---

static void discover_next_service_characteristics(void);

static int on_command_written(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "Camera acknowledged the characteristic write");
    } else {
        ESP_LOGE(TAG, "Camera rejected the characteristic write: %d",
                 error->status);
    }
    return 0;
}

static void write_command(void)
{
    size_t command_len;
    uint8_t *command = camera_ble_protocol_command(&command_len);
    ESP_LOGI(TAG, "Found characteristic 0x%04x at handle %u",
             WIFI_CONTROL_CHAR_UUID, s_endpoints.control_value_handle);
    if (command_len == 0) {
        ESP_LOGW(TAG,
                 "No payload configured; discovery complete, no write sent");
        return;
    }
    int rc;
    if (s_endpoints.control_write_with_response) {
        rc = ble_gattc_write_flat(
            s_conn_handle, s_endpoints.control_value_handle, command,
            (uint16_t)command_len, on_command_written, NULL);
    } else {
        rc = ble_gattc_write_no_rsp_flat(s_conn_handle,
                                         s_endpoints.control_value_handle,
                                         command, (uint16_t)command_len);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT write request failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Wi-Fi enable payload submitted (%u bytes)",
                 (unsigned)command_len);
    }
}

static int on_cccd_written(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    BleLock lock;
    if (error->status == 0) {
        ESP_LOGI(TAG, "Characteristic update subscription confirmed");
    } else {
        ESP_LOGE(TAG, "Characteristic update subscription failed: %d",
                 error->status);
    }
    write_command();
    return 0;
}

static int on_descriptor(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    BleLock lock;
    if (error->status == 0) {
        if (uuid16_of(&dsc->uuid) == CLIENT_CHARACTERISTIC_CONFIG_UUID) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Descriptor discovery failed: %d", error->status);
    }
    if (s_cccd_handle == 0) {
        ESP_LOGE(TAG, "Client configuration descriptor not found for handle %u",
                 s_endpoints.response_value_handle);
        write_command();
        return 0;
    }

    const uint16_t config = s_endpoints.response_cccd_value;
    const uint8_t value[2] = {(uint8_t)(config & 0xff), (uint8_t)(config >> 8)};
    ESP_LOGI(TAG, "Registering for %s on characteristic handle %u",
             config == (camera::ble::kCccdNotify | camera::ble::kCccdIndicate)
                 ? "notifications and indications"
                 : (config == camera::ble::kCccdNotify ? "notifications"
                                                       : "indications"),
             s_endpoints.response_value_handle);
    int rc = ble_gattc_write_flat(s_conn_handle, s_cccd_handle, value,
                                  sizeof(value), on_cccd_written, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not enable updates for handle %u: %d",
                 s_endpoints.response_value_handle, rc);
        write_command();
    }
    return 0;
}

static void on_profile_discovered(void)
{
    s_endpoints =
        s_profile.find_camera(CAMERA_BLE_SERVICE_UUID, WIFI_CONTROL_CHAR_UUID,
                              WIFI_RESPONSE_CHAR_UUID);
    if (!s_endpoints.found || s_profile.overflow()) {
        ESP_LOGI(TAG,
                 "BLE device is not compatible: service 0x%04x has no "
                 "writable 0x%04x and notify/indicate 0x%04x pair",
                 CAMERA_BLE_SERVICE_UUID, WIFI_CONTROL_CHAR_UUID,
                 WIFI_RESPONSE_CHAR_UUID);
        disconnect_camera("GATT characteristics do not match");
        return;
    }
    ESP_LOGI(TAG, "BLE device matches the expected camera GATT profile");
    s_cccd_handle = 0;
    int rc = ble_gattc_disc_all_dscs(
        s_conn_handle, s_endpoints.response_value_handle,
        s_endpoints.response_end_handle, on_descriptor, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not discover descriptors: %d", rc);
        write_command();
    }
}

static int on_characteristic(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    BleLock lock;
    if (error->status == 0) {
        ESP_LOGI(TAG, "Characteristic handle=%u properties=0x%02x",
                 chr->val_handle, chr->properties);
        log_uuid("characteristic", &chr->uuid);
        s_profile.add_characteristic({s_discovery_service_index,
                                      uuid16_of(&chr->uuid), chr->def_handle,
                                      chr->val_handle, chr->properties});
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        const camera::ble::Service &service =
            s_profile.service(s_discovery_service_index);
        ESP_LOGE(TAG,
                 "Could not read characteristics for GATT service handles "
                 "%u-%u: %d",
                 service.start_handle, service.end_handle, error->status);
    }
    s_discovery_service_index++;
    discover_next_service_characteristics();
    return 0;
}

static void discover_next_service_characteristics(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (s_discovery_service_index >= s_profile.service_count()) {
        on_profile_discovered();
        return;
    }
    const camera::ble::Service &service =
        s_profile.service(s_discovery_service_index);
    int rc =
        ble_gattc_disc_all_chrs(s_conn_handle, service.start_handle,
                                service.end_handle, on_characteristic, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not discover characteristics: %d", rc);
        disconnect_camera("Characteristic discovery could not be started");
    }
}

static int on_service(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)arg;
    BleLock lock;
    if (error->status == 0) {
        log_uuid("service", &service->uuid);
        const uint16_t uuid16 = uuid16_of(&service->uuid);
        if (uuid16 == CAMERA_BLE_SERVICE_UUID) {
            ESP_LOGI(TAG,
                     "Device=\"%s\": found camera service 0x%04x "
                     "(handles %u-%u)",
                     s_device_name, CAMERA_BLE_SERVICE_UUID,
                     service->start_handle, service->end_handle);
        }
        if (!s_profile.add_service(
                {uuid16, service->start_handle, service->end_handle})) {
            ESP_LOGW(TAG, "Too many GATT services to inspect");
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "GATT service discovery failed: %d", error->status);
        disconnect_camera("GATT service discovery failed");
        return 0;
    }
    s_discovery_service_index = 0;
    discover_next_service_characteristics();
    return 0;
}

// --- GAP events ---

static void handle_notification(const struct ble_gap_event *event)
{
    uint8_t data[MAX_NOTIFICATION_LEN];
    uint16_t length = 0;
    int rc =
        ble_hs_mbuf_to_flat(event->notify_rx.om, data, sizeof(data), &length);
    ESP_LOGI(TAG, "Received %s on handle %u (%u bytes)",
             event->notify_rx.indication ? "indication" : "notification",
             event->notify_rx.attr_handle, length);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE update payload is unavailable or too long");
    } else if (event->notify_rx.attr_handle ==
               s_endpoints.response_value_handle) {
        camera_ble_protocol_process_notification(data, length);
    } else {
        ESP_LOGI(TAG, "No decoder registered for notification handle %u",
                 event->notify_rx.attr_handle);
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    BleLock lock;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_advertisement(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (s_activation_enabled && !s_connecting) {
            ESP_LOGI(TAG, "%s not found; restarting scan",
                     CAMERA_BLE_TARGET_NAME);
        }
        start_scan();
        break;
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE device connection failed: %d",
                     event->connect.status);
            connection_ended();
            start_scan();
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "Connected to %s; discovering GATT services",
                 s_device_name);
        if (!s_activation_enabled) {
            disconnect_camera("BLE activation was paused");
            break;
        }
        s_profile.reset();
        camera_ble_protocol_reset_response();
        s_endpoints = CameraEndpoints();
        if (ble_gattc_disc_all_svcs(s_conn_handle, on_service, NULL) != 0) {
            disconnect_camera("Service discovery could not be started");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE device disconnected (reason 0x%03x)",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_endpoints = CameraEndpoints();
        camera_ble_protocol_reset_response();
        connection_ended();
        if (s_activation_enabled) {
            ESP_LOGI(TAG, "Restarting scan for %s", CAMERA_BLE_TARGET_NAME);
        }
        start_scan();
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_notification(event);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU is %u", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

// --- Stack lifecycle ---

static void on_sync(void)
{
    BleLock lock;
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not determine the BLE address type: %d", rc);
        return;
    }
    s_synced = true;
    start_scan();
}

static void on_reset(int reason)
{
    BleLock lock;
    ESP_LOGW(TAG, "NimBLE host reset (reason %d)", reason);
    s_synced = false;
    s_scanning = false;
    s_connecting = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    connection_ended();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
// The Bluetooth controller runs on the ESP32-C6 co-processor and is switched
// on and off over ESP-Hosted. Co-processor firmware older than ESP-Hosted
// 2.5.2 keeps the controller enabled and does not answer these requests;
// each attempt then blocks until the RPC times out. After the first failure
// the controller is assumed to be always on and is no longer switched.
static bool s_remote_controller_switchable = true;

static void start_remote_controller(void)
{
    if (!s_remote_controller_switchable) {
        return;
    }
    esp_err_t error = esp_hosted_bt_controller_init();
    if (error == ESP_OK) {
        error = esp_hosted_bt_controller_enable();
    }
    if (error != ESP_OK) {
        s_remote_controller_switchable = false;
        ESP_LOGW(TAG,
                 "Could not enable the co-processor BLE controller (%s); "
                 "assuming its firmware keeps BLE enabled. Update the "
                 "ESP-Hosted firmware on the co-processor to 2.5.2 or later.",
                 esp_err_to_name(error));
    }
}

static void stop_remote_controller(void)
{
    if (!s_remote_controller_switchable) {
        return;
    }
    esp_err_t error = esp_hosted_bt_controller_disable();
    if (error == ESP_OK) {
        // Keep the controller memory so BLE can be started again.
        error = esp_hosted_bt_controller_deinit(false);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable the co-processor BLE controller: %s",
                 esp_err_to_name(error));
    }
}
#endif

static esp_err_t start_stack(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    start_remote_controller();
#endif
    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize NimBLE: %s",
                 esp_err_to_name(error));
        return error;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    {
        BleLock lock;
        s_stack_running = true;
    }
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE stack started");
    return ESP_OK;
}

static void stop_stack(void)
{
    // Must be called without the lock: stopping waits for the host task,
    // which may need the lock to finish its callbacks.
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not stop the BLE host: %d", rc);
        BleLock lock;
        set_link_active(false);
        return;
    }
    esp_err_t error = nimble_port_deinit();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not release the BLE stack: %s",
                 esp_err_to_name(error));
    }
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    stop_remote_controller();
#endif
    BleLock lock;
    s_stack_running = false;
    s_synced = false;
    s_scanning = false;
    s_connecting = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_endpoints = CameraEndpoints();
    set_link_active(false);
    ESP_LOGI(TAG, "BLE stack released");
}

// --- Public API ---

esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!camera_ble_protocol_init(on_credentials)) {
        ESP_LOGE(TAG,
                 "Invalid Wi-Fi enable payload; expected up to %u hex bytes",
                 CAMERA_BLE_MAX_COMMAND_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    size_t command_len;
    camera_ble_protocol_command(&command_len);
    if (command_len == 0) {
        ESP_LOGW(TAG,
                 "Payload is empty; this run will not write to the camera");
    }
    snprintf(s_device_name, sizeof(s_device_name), "unknown");
    ESP_LOGI(TAG, "BLE camera activation ready; target camera is %s",
             CAMERA_BLE_TARGET_NAME);
    return ESP_OK;
}

void camera_ble_set_link_callback(camera_ble_link_cb_t callback)
{
    s_link_callback = callback;
}

void camera_ble_resume(void)
{
    bool start = false;
    {
        BleLock lock;
        if (s_activation_enabled) {
            return;
        }
        s_activation_enabled = true;
        ESP_LOGI(TAG, "BLE camera activation resumed");
        if (!s_stack_running) {
            start = true;
        } else if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            // Reconnect so the Wi-Fi-enable command is sent again.
            disconnect_camera("Restarting camera activation");
        } else {
            start_scan();
        }
    }
    if (start) {
        start_stack();
    }
}

void camera_ble_pause(void)
{
    bool release = false;
    {
        BleLock lock;
        if (!s_activation_enabled) {
            return;
        }
        s_activation_enabled = false;
        if (s_scanning) {
            ble_gap_disc_cancel();
            s_scanning = false;
        }
        release = kReleaseAfterActivation && s_stack_running;
        if (!release) {
            ESP_LOGI(TAG, "BLE camera activation paused; an existing camera "
                          "connection is kept");
        }
    }
    if (release) {
        ESP_LOGI(TAG, "BLE camera activation paused; releasing the BLE stack");
        stop_stack();
    }
}
