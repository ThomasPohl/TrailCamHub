#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "camera_ble.h"
#include "camera_ble_discovery.h"
#include "camera_ble_profile.h"
#include "camera_ble_protocol.h"

#define WIFI_CONTROL_CHAR_UUID 0x0002
#define SCAN_DURATION_SECONDS 30
#define MAX_BLE_DATA_LEN 31
#define MAX_BLE_DEVICE_NAME_LEN 32
#define MAX_SURVEY_DEVICES 16
#define CLIENT_CHARACTERISTIC_CONFIG_UUID 0x2902

typedef enum {
    BLE_SCAN_STOPPED,
    BLE_SCAN_STARTING,
    BLE_SCAN_ACTIVE,
    BLE_SCAN_STOPPING,
} ble_scan_state_t;

typedef enum {
    BLE_PHASE_SURVEY,
    BLE_PHASE_TARGET_CAMERA,
} ble_phase_t;

typedef struct {
    esp_bd_addr_t address;
    esp_ble_addr_type_t address_type;
    char name[MAX_BLE_DEVICE_NAME_LEN];
} survey_device_t;

static const char *TAG = "trailcamhub_ble";
static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

static esp_gatt_if_t s_gatt_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id;
static ble_scan_state_t s_scan_state = BLE_SCAN_STOPPED;
static ble_phase_t s_phase = BLE_PHASE_SURVEY;
static survey_device_t s_survey_devices[MAX_SURVEY_DEVICES];
static size_t s_survey_device_count;
static size_t s_survey_device_index;
static bool s_survey_overflow;
static bool s_connection_open_pending;
static bool s_connection_active;
static bool s_disconnected_while_opening;
static esp_bd_addr_t s_camera_address;
static esp_ble_addr_type_t s_camera_address_type;
static bool s_device_selected;
static bool s_write_with_response;
static size_t s_notification_index;

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
                                   i == 0 ? "%02X" : " %02X",
                                   data[offset + i]);
            if (written < 0 ||
                (size_t)written >= sizeof(formatted) - formatted_len) {
                ESP_LOGE(TAG, "Could not format %s", label);
                return;
            }
            formatted_len += (size_t)written;
        }

        if (length <= MAX_BLE_DATA_LEN) {
            ESP_LOGI(TAG, "%s (%u bytes): %s", label, (unsigned)length,
                     formatted);
        } else {
            ESP_LOGI(TAG, "%s bytes %u-%u of %u: %s", label,
                     (unsigned)offset, (unsigned)(offset + chunk_len - 1),
                     (unsigned)length, formatted);
        }
    }
}

static void log_ble_device(const esp_ble_gap_cb_param_t *param)
{
    const uint8_t *address = param->scan_rst.bda;
    size_t adv_len = param->scan_rst.adv_data_len;
    size_t scan_rsp_len = param->scan_rst.scan_rsp_len;

    ESP_LOGI(TAG,
             "BLE device advertisement: address=%02X:%02X:%02X:%02X:%02X:%02X "
             "address_type=%u RSSI=%d dBm event_type=%u",
             address[0], address[1], address[2], address[3], address[4],
             address[5], (unsigned)param->scan_rst.ble_addr_type,
             param->scan_rst.rssi, (unsigned)param->scan_rst.ble_evt_type);
    if (adv_len + scan_rsp_len > sizeof(param->scan_rst.ble_adv)) {
        ESP_LOGW(TAG, "BLE advertisement data lengths exceed the available buffer");
        return;
    }

    log_hex_data("BLE advertising data", param->scan_rst.ble_adv, adv_len);
    if (scan_rsp_len > 0) {
        log_hex_data("BLE scan response",
                     param->scan_rst.ble_adv + adv_len, scan_rsp_len);
    }
}

static void get_ble_device_name(esp_ble_gap_cb_param_t *param, char *name,
                                size_t name_capacity)
{
    uint8_t name_len = 0;
    uint8_t *advertised_name = esp_ble_resolve_adv_data(
        param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (advertised_name == NULL) {
        advertised_name = esp_ble_resolve_adv_data(
            param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }

    if (advertised_name == NULL || name_len == 0) {
        snprintf(name, name_capacity, "unknown");
        return;
    }

    size_t copy_len = name_len < name_capacity - 1
                          ? name_len
                          : name_capacity - 1;
    memcpy(name, advertised_name, copy_len);
    name[copy_len] = '\0';
}

static void start_scan(void)
{
    if (s_scan_state != BLE_SCAN_STOPPED) {
        ESP_LOGD(TAG, "Ignoring scan request while scan state is %d",
                 s_scan_state);
        return;
    }

    esp_err_t error = esp_ble_gap_start_scanning(SCAN_DURATION_SECONDS);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start BLE scan: %s", esp_err_to_name(error));
        return;
    }
    s_scan_state = BLE_SCAN_STARTING;
}

static void clear_device_profile(void)
{
    camera_ble_profile_reset();
    s_notification_index = 0;
    camera_ble_protocol_reset_response();
}

static void start_next_survey_device(void);

static void disconnect_current_device(const char *reason)
{
    ESP_LOGI(TAG, "%s; disconnecting BLE device", reason);
    esp_err_t error = esp_ble_gap_disconnect(s_camera_address);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not disconnect BLE device: %s",
                 esp_err_to_name(error));
        s_connection_active = false;
        s_device_selected = false;
        if (s_phase == BLE_PHASE_SURVEY) {
            start_next_survey_device();
        } else {
            start_scan();
        }
    }
}

static void start_next_characteristic_subscription(void);
static void find_control_characteristic(void);

static void complete_characteristic_subscription(void)
{
    s_notification_index++;
    start_next_characteristic_subscription();
}

static void enable_characteristic_updates(uint16_t characteristic_handle)
{
    esp_bt_uuid_t config_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = CLIENT_CHARACTERISTIC_CONFIG_UUID,
    };
    esp_gattc_descr_elem_t descriptor;
    uint16_t count = 1;
    uint16_t config =
        camera_ble_profile_notification_config(characteristic_handle);
    uint8_t value[2] = {(uint8_t)(config & 0xff), (uint8_t)(config >> 8)};
    esp_err_t status;

    if (config == 0) {
        ESP_LOGW(TAG, "No update configuration found for handle %u",
                 characteristic_handle);
        complete_characteristic_subscription();
        return;
    }

    status = esp_ble_gattc_get_descr_by_char_handle(
        s_gatt_if, s_conn_id, characteristic_handle, config_uuid, &descriptor,
        &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGE(TAG, "Client configuration descriptor not found for handle %u",
                 characteristic_handle);
        complete_characteristic_subscription();
        return;
    }

    status = esp_ble_gattc_write_char_descr(
        s_gatt_if, s_conn_id, descriptor.handle, sizeof(value), value,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "Could not enable updates for handle %u: %d",
                 characteristic_handle, status);
        complete_characteristic_subscription();
    } else {
        ESP_LOGI(TAG, "Enabling updates for characteristic handle %u",
                 characteristic_handle);
    }
}

static void start_next_characteristic_subscription(void)
{
    if (s_notification_index >= camera_ble_profile_notification_count()) {
        find_control_characteristic();
        return;
    }

    uint16_t handle;
    uint16_t config;
    if (!camera_ble_profile_notification(s_notification_index, &handle,
                                         &config)) {
        ESP_LOGE(TAG, "Could not read queued characteristic update");
        complete_characteristic_subscription();
        return;
    }
    esp_err_t error = esp_ble_gattc_register_for_notify(
        s_gatt_if, s_camera_address, handle);

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not register for updates on handle %u: %s",
                 handle, esp_err_to_name(error));
        complete_characteristic_subscription();
        return;
    }

    ESP_LOGI(TAG, "Registering for %s on characteristic handle %u",
             config == 0x0003 ? "notifications and indications" :
             (config == 0x0001 ? "notifications" : "indications"), handle);
}

static void collect_survey_device(esp_ble_gap_cb_param_t *param)
{
    if (!camera_ble_is_connectable_advertisement(
            param->scan_rst.ble_evt_type)) {
        ESP_LOGI(TAG,
                 "BLE device %02X:%02X:%02X:%02X:%02X:%02X is not "
                 "connectable (event_type=%u); GATT profile unavailable",
                 param->scan_rst.bda[0], param->scan_rst.bda[1],
                 param->scan_rst.bda[2], param->scan_rst.bda[3],
                 param->scan_rst.bda[4], param->scan_rst.bda[5],
                 (unsigned)param->scan_rst.ble_evt_type);
        return;
    }
    if (!camera_ble_is_candidate_advertisement(param)) {
        ESP_LOGD(TAG,
                 "Ignoring BLE device %02X:%02X:%02X:%02X:%02X:%02X because "
                 "it does not match known camera name/UUID filters",
                 param->scan_rst.bda[0], param->scan_rst.bda[1],
                 param->scan_rst.bda[2], param->scan_rst.bda[3],
                 param->scan_rst.bda[4], param->scan_rst.bda[5]);
        return;
    }

    for (size_t i = 0; i < s_survey_device_count; i++) {
        if (s_survey_devices[i].address_type ==
                param->scan_rst.ble_addr_type &&
            memcmp(s_survey_devices[i].address, param->scan_rst.bda,
                   sizeof(esp_bd_addr_t)) == 0) {
            return;
        }
    }
    if (s_survey_device_count >= MAX_SURVEY_DEVICES) {
        if (!s_survey_overflow) {
            ESP_LOGW(TAG, "Survey device limit (%u) reached; "
                     "additional devices will be skipped",
                     MAX_SURVEY_DEVICES);
            s_survey_overflow = true;
        }
        return;
    }

    log_ble_device(param);
    survey_device_t *device = &s_survey_devices[s_survey_device_count++];
    memcpy(device->address, param->scan_rst.bda, sizeof(device->address));
    device->address_type = param->scan_rst.ble_addr_type;
    get_ble_device_name(param, device->name, sizeof(device->name));
    ESP_LOGI(TAG, "Queued BLE device for GATT profile inspection (%u/%u)",
             (unsigned)s_survey_device_count, MAX_SURVEY_DEVICES);
}

static bool was_surveyed_connectable_device(
    const esp_ble_gap_cb_param_t *param)
{
    for (size_t i = 0; i < s_survey_device_count; i++) {
        if (s_survey_devices[i].address_type ==
                param->scan_rst.ble_addr_type &&
            memcmp(s_survey_devices[i].address, param->scan_rst.bda,
                   sizeof(esp_bd_addr_t)) == 0) {
            return true;
        }
    }
    return false;
}

static void select_target_camera(esp_ble_gap_cb_param_t *param)
{
    bool connectable = camera_ble_is_connectable_advertisement(
        param->scan_rst.ble_evt_type);
    bool known_scan_response =
        param->scan_rst.ble_evt_type == ESP_BLE_EVT_SCAN_RSP &&
        was_surveyed_connectable_device(param);
    if (!connectable && !known_scan_response) {
        ESP_LOGD(TAG, "Ignoring non-connectable advertisement for target name");
        return;
    }
    if (!camera_ble_is_candidate_advertisement(param)) {
        ESP_LOGD(TAG, "Ignoring BLE device that does not match camera filters");
        return;
    }

    log_ble_device(param);
    memcpy(s_camera_address, param->scan_rst.bda, sizeof(s_camera_address));
    s_camera_address_type = param->scan_rst.ble_addr_type;
    s_device_selected = true;
    s_disconnected_while_opening = false;
    clear_device_profile();
    char device_name[MAX_BLE_DEVICE_NAME_LEN];
    get_ble_device_name(param, device_name, sizeof(device_name));
    camera_ble_profile_set_device_name(device_name);
    ESP_LOGI(TAG,
             "Found compatible camera candidate; stopping scan before connection");
    s_scan_state = BLE_SCAN_STOPPING;
    esp_err_t error = esp_ble_gap_stop_scanning();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not stop BLE scan: %s", esp_err_to_name(error));
        s_scan_state = BLE_SCAN_ACTIVE;
        s_device_selected = false;
    }
}

static void start_next_survey_device(void)
{
    if (s_phase != BLE_PHASE_SURVEY || s_connection_open_pending ||
        s_connection_active) {
        return;
    }

    if (s_survey_device_index >= s_survey_device_count) {
        s_phase = BLE_PHASE_TARGET_CAMERA;
        s_device_selected = false;
        ESP_LOGI(TAG, "GATT survey complete (%u device(s)); scanning for %s",
                 (unsigned)s_survey_device_count, CAMERA_BLE_TARGET_NAME);
        start_scan();
        return;
    }

    survey_device_t *device = &s_survey_devices[s_survey_device_index++];
    memcpy(s_camera_address, device->address, sizeof(s_camera_address));
    s_camera_address_type = device->address_type;
    s_device_selected = true;
    clear_device_profile();
    camera_ble_profile_set_device_name(device->name);
    s_disconnected_while_opening = false;
    s_connection_open_pending = true;
    ESP_LOGI(TAG, "Inspecting GATT profile for device \"%s\" (%u/%u)",
             device->name, (unsigned)s_survey_device_index,
             (unsigned)s_survey_device_count);

    esp_err_t error = esp_ble_gattc_open(
        s_gatt_if, s_camera_address, s_camera_address_type, true);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not connect to surveyed BLE device: %s",
                 esp_err_to_name(error));
        s_connection_open_pending = false;
        s_device_selected = false;
        start_next_survey_device();
    }
}

static void find_control_characteristic(void)
{
    size_t command_len;
    uint8_t *command = camera_ble_protocol_command(&command_len);
    esp_bt_uuid_t char_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = WIFI_CONTROL_CHAR_UUID,
    };
    esp_gattc_char_elem_t characteristic;
    uint16_t count = 1;
    esp_err_t status;

    uint16_t service_start = camera_ble_profile_service_start_handle();
    uint16_t service_end = camera_ble_profile_service_end_handle();
    if (service_start == 0 || service_end == 0) {
        ESP_LOGE(TAG, "Camera service 0x%04x was not found",
                 CAMERA_BLE_SERVICE_UUID);
        return;
    }

    status = esp_ble_gattc_get_char_by_uuid(s_gatt_if, s_conn_id,
                                            service_start, service_end, char_uuid,
                                            &characteristic, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGE(TAG, "Writable characteristic 0x%04x was not found",
                 WIFI_CONTROL_CHAR_UUID);
        return;
    }

    ESP_LOGI(TAG,
             "Found characteristic 0x%04x at handle %u (properties 0x%02x)",
             WIFI_CONTROL_CHAR_UUID, characteristic.char_handle,
             characteristic.properties);
    if ((characteristic.properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0) {
        s_write_with_response = true;
    } else if ((characteristic.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
        s_write_with_response = false;
    } else {
        ESP_LOGE(TAG, "Characteristic 0x%04x is not writable",
                 WIFI_CONTROL_CHAR_UUID);
        return;
    }

    if (command_len == 0) {
        ESP_LOGW(TAG, "No payload configured; discovery complete, no write sent");
        return;
    }

    status = esp_ble_gattc_write_char(
        s_gatt_if, s_conn_id, characteristic.char_handle, (uint16_t)command_len,
        command,
        s_write_with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "GATT write request failed with status %d", status);
    } else {
        ESP_LOGI(TAG, "Wi-Fi enable payload submitted (%u bytes)",
                 (unsigned)command_len);
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        start_scan();
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scan_state = BLE_SCAN_ACTIVE;
            if (s_phase == BLE_PHASE_SURVEY) {
                ESP_LOGI(TAG, "Surveying connectable BLE devices for %u seconds",
                         SCAN_DURATION_SECONDS);
            } else {
                ESP_LOGI(TAG, "Scanning for BLE camera %s",
                         CAMERA_BLE_TARGET_NAME);
            }
        } else {
            s_scan_state = BLE_SCAN_STOPPED;
            ESP_LOGE(TAG, "BLE scan start failed: %d",
                     param->scan_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (s_scan_state != BLE_SCAN_ACTIVE) {
            break;
        }
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if (s_phase == BLE_PHASE_SURVEY) {
                collect_survey_device(param);
            } else if (!s_device_selected &&
                       camera_ble_is_candidate_advertisement(param)) {
                select_target_camera(param);
            }
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s_scan_state = BLE_SCAN_STOPPED;
            if (s_phase == BLE_PHASE_SURVEY) {
                ESP_LOGI(TAG, "BLE survey scan complete; inspecting %u "
                         "connectable device(s)",
                         (unsigned)s_survey_device_count);
                s_survey_device_index = 0;
                start_next_survey_device();
            } else {
                ESP_LOGI(TAG, "%s not found; restarting target scan",
                         CAMERA_BLE_TARGET_NAME);
                start_scan();
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "BLE scan stop failed: %d",
                     param->scan_stop_cmpl.status);
            s_device_selected = false;
            s_scan_state = BLE_SCAN_ACTIVE;
            ESP_LOGW(TAG, "Keeping current scan active after stop failure");
        } else {
            s_scan_state = BLE_SCAN_STOPPED;
            if (s_device_selected) {
                s_connection_open_pending = true;
                s_disconnected_while_opening = false;
                esp_err_t error = esp_ble_gattc_open(
                    s_gatt_if, s_camera_address, s_camera_address_type, true);
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "Could not connect to BLE device: %s",
                             esp_err_to_name(error));
                    s_connection_open_pending = false;
                    s_device_selected = false;
                    start_scan();
                }
            } else {
                ESP_LOGW(TAG, "Scan stopped without a selected target device");
                start_scan();
            }
        }
        break;
    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATT client registration failed: %d", param->reg.status);
            break;
        }
        s_gatt_if = gattc_if;
        if (esp_ble_gap_set_scan_params(&scan_params) != ESP_OK) {
            ESP_LOGE(TAG, "Could not configure BLE scan");
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "BLE device connection failed: %d",
                     param->open.status);
            s_connection_open_pending = false;
            s_device_selected = false;
            if (s_phase == BLE_PHASE_SURVEY) {
                start_next_survey_device();
            } else {
                start_scan();
            }
            break;
        }
        s_conn_id = param->open.conn_id;
        s_connection_open_pending = false;
        s_connection_active = true;
        ESP_LOGI(TAG, "Connected to %s; discovering GATT services",
                 s_phase == BLE_PHASE_SURVEY ? "surveyed BLE device" :
                 CAMERA_BLE_TARGET_NAME);
        if (s_disconnected_while_opening) {
            s_connection_active = false;
            s_device_selected = false;
            if (s_phase == BLE_PHASE_SURVEY) {
                start_next_survey_device();
            } else {
                start_scan();
            }
            break;
        }
        esp_err_t search_error = esp_ble_gattc_search_service(
            gattc_if, s_conn_id, NULL);
        if (search_error != ESP_OK) {
            ESP_LOGE(TAG, "Could not start GATT service discovery: %s",
                     esp_err_to_name(search_error));
            disconnect_current_device("Service discovery could not be started");
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        camera_ble_profile_add_service(
            &param->search_res.srvc_id.uuid,
            param->search_res.start_handle, param->search_res.end_handle);
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATT service discovery failed: %d",
                     param->search_cmpl.status);
            disconnect_current_device("GATT service discovery failed");
        } else if (s_phase == BLE_PHASE_SURVEY) {
            camera_ble_profile_discover_characteristics(gattc_if, s_conn_id);
            disconnect_current_device("GATT profile inspection complete");
        } else if (!camera_ble_profile_discover_characteristics(gattc_if,
                                                                s_conn_id)) {
            disconnect_current_device("GATT characteristics do not match");
        } else {
            s_notification_index = 0;
            start_next_characteristic_subscription();
        }
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Registered for updates on handle %u",
                     param->reg_for_notify.handle);
            enable_characteristic_updates(param->reg_for_notify.handle);
        } else {
            ESP_LOGE(TAG, "Could not register for updates on handle %u: %d",
                     param->reg_for_notify.handle,
                     param->reg_for_notify.status);
            complete_characteristic_subscription();
        }
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Characteristic update subscription confirmed");
        } else {
            ESP_LOGE(TAG, "Characteristic update subscription failed: %d",
                     param->write.status);
        }
        complete_characteristic_subscription();
        break;
    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(TAG, "Received %s on handle %u (%u bytes)",
                 param->notify.is_notify ? "notification" : "indication",
                 param->notify.handle, param->notify.value_len);
        if (param->notify.value_len > 0 && param->notify.value == NULL) {
            ESP_LOGE(TAG, "BLE update payload is unavailable");
        } else if (param->notify.handle ==
                   camera_ble_profile_wifi_response_handle()) {
            camera_ble_protocol_process_notification(
                param->notify.value, param->notify.value_len);
        } else {
            ESP_LOGI(TAG, "No decoder registered for notification handle %u",
                     param->notify.handle);
        }
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (s_write_with_response) {
            if (param->write.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "Camera acknowledged the characteristic write");
            } else {
                ESP_LOGE(TAG, "Camera rejected the characteristic write: %d",
                         param->write.status);
            }
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "BLE device disconnected");
        if (s_connection_open_pending) {
            s_disconnected_while_opening = true;
            break;
        }
        if (!s_connection_active) {
            ESP_LOGD(TAG, "Ignoring disconnect callback without an active connection");
            break;
        }
        s_connection_active = false;
        s_device_selected = false;
        clear_device_profile();
        if (s_phase == BLE_PHASE_SURVEY) {
            start_next_survey_device();
        } else {
            ESP_LOGI(TAG, "Restarting scan for %s",
                     CAMERA_BLE_TARGET_NAME);
            start_scan();
        }
        break;
    default:
        break;
    }
}

esp_err_t camera_ble_start(void)
{
    if (!camera_ble_protocol_init()) {
        ESP_LOGE(TAG, "Invalid Wi-Fi enable payload; expected up to %u hex bytes",
                 CAMERA_BLE_MAX_COMMAND_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting BLE survey; target camera is %s",
             CAMERA_BLE_TARGET_NAME);
    size_t command_len;
    camera_ble_protocol_command(&command_len);
    if (command_len == 0) {
        ESP_LOGW(TAG, "Payload is empty; this run will not write to the camera");
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), TAG,
                        "Could not register BLE GAP callback");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(gattc_event_handler), TAG,
                        "Could not register BLE GATT client callback");
    return esp_ble_gattc_app_register(0);
}
