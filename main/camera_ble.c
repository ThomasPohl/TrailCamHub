#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "cJSON.h"

#include "camera_ble.h"

#ifndef CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX
#define CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX "42545F4B45595F4F4E"
#endif

#define CAMERA_SERVICE_UUID 0x8801
#define WIFI_CONTROL_CHAR_UUID 0x0002
#define MAX_COMMAND_LEN 20
#define SCAN_DURATION_SECONDS 30
#define MAX_BLE_DATA_LEN 31
#define MAX_DISCOVERED_SERVICES 32
#define MAX_NOTIFICATION_CHARACTERISTICS 16
#define CLIENT_CHARACTERISTIC_CONFIG_UUID 0x2902
#define WIFI_RESPONSE_CHAR_UUID 0x0003
#define WIFI_RESPONSE_HEADER_LEN 8
#define MAX_WIFI_RESPONSE_JSON_LEN 512
#define MAX_WIFI_RESPONSE_FRAME_LEN \
    (WIFI_RESPONSE_HEADER_LEN + MAX_WIFI_RESPONSE_JSON_LEN)
#define WIFI_RESPONSE_MAGIC_0 0x13
#define WIFI_RESPONSE_MAGIC_1 0x57

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    esp_bt_uuid_t uuid;
} discovered_service_t;

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
static discovered_service_t s_discovered_services[MAX_DISCOVERED_SERVICES];
static size_t s_discovered_service_count;
static bool s_discovered_service_overflow;
static uint16_t s_service_start_handle;
static uint16_t s_service_end_handle;
static uint8_t s_command[MAX_COMMAND_LEN];
static size_t s_command_len;
static esp_bd_addr_t s_camera_address;
static esp_ble_addr_type_t s_camera_address_type;
static bool s_device_selected;
static bool s_write_with_response;
static uint16_t s_notification_handles[MAX_NOTIFICATION_CHARACTERISTICS];
static uint16_t s_notification_configs[MAX_NOTIFICATION_CHARACTERISTICS];
static size_t s_notification_count;
static size_t s_notification_index;
static uint16_t s_wifi_response_handle;
static uint8_t s_wifi_response_frame[MAX_WIFI_RESPONSE_FRAME_LEN];
static size_t s_wifi_response_frame_len;
static size_t s_wifi_response_expected_len;

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

static void log_gatt_uuid(const char *kind, const esp_bt_uuid_t *uuid)
{
    if (uuid->len == ESP_UUID_LEN_16) {
        ESP_LOGI(TAG, "Discovered GATT %s UUID=0x%04x", kind,
                 uuid->uuid.uuid16);
    } else if (uuid->len == ESP_UUID_LEN_32) {
        ESP_LOGI(TAG, "Discovered GATT %s UUID=0x%08lx", kind,
                 (unsigned long)uuid->uuid.uuid32);
    } else if (uuid->len == ESP_UUID_LEN_128) {
        ESP_LOGI(TAG, "Discovered GATT %s with a 128-bit UUID", kind);
        log_hex_data("GATT UUID bytes", uuid->uuid.uuid128, ESP_UUID_LEN_128);
    } else {
        ESP_LOGW(TAG, "Discovered GATT %s with unsupported UUID length %u",
                 kind, (unsigned)uuid->len);
    }
}

static void reset_wifi_response_frame(void)
{
    s_wifi_response_frame_len = 0;
    s_wifi_response_expected_len = 0;
}

static void log_wifi_response_json(const uint8_t *json, size_t json_len,
                                   uint16_t message_type)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json, json_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "Could not parse Wi-Fi response JSON");
        cJSON_Delete(root);
        return;
    }

    cJSON *ret = cJSON_GetObjectItemCaseSensitive(root, "ret");
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *bssid = cJSON_GetObjectItemCaseSensitive(root, "bssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "pwd");

    ESP_LOGI(TAG, "Parsed Wi-Fi response (message_type=0x%04x, %u JSON bytes)",
             message_type, (unsigned)json_len);
    if (cJSON_IsNumber(ret)) {
        ESP_LOGI(TAG, "Camera response status: %d", ret->valueint);
    } else {
        ESP_LOGW(TAG, "Wi-Fi response has no numeric ret field");
    }
    if (cJSON_IsString(ssid) && ssid->valuestring != NULL) {
        ESP_LOGI(TAG, "Camera Wi-Fi SSID: %s", ssid->valuestring);
    } else {
        ESP_LOGW(TAG, "Wi-Fi response has no string ssid field");
    }
    if (cJSON_IsString(bssid) && bssid->valuestring != NULL) {
        ESP_LOGI(TAG, "Camera Wi-Fi BSSID: %s", bssid->valuestring);
    } else {
        ESP_LOGW(TAG, "Wi-Fi response has no string bssid field");
    }
    if (cJSON_IsString(password) && password->valuestring != NULL) {
        ESP_LOGI(TAG, "Camera Wi-Fi password: [REDACTED]");
    } else {
        ESP_LOGW(TAG, "Wi-Fi response has no string pwd field");
    }

    cJSON_Delete(root);
}

static void complete_wifi_response_frame(void)
{
    size_t json_len = s_wifi_response_expected_len - WIFI_RESPONSE_HEADER_LEN;
    uint16_t message_type = (uint16_t)s_wifi_response_frame[2] |
                            ((uint16_t)s_wifi_response_frame[3] << 8);

    log_wifi_response_json(s_wifi_response_frame + WIFI_RESPONSE_HEADER_LEN,
                           json_len, message_type);
    reset_wifi_response_frame();
}

static void process_wifi_response_byte(uint8_t byte)
{
    if (s_wifi_response_frame_len == 0) {
        if (byte == WIFI_RESPONSE_MAGIC_0) {
            s_wifi_response_frame[s_wifi_response_frame_len++] = byte;
        }
        return;
    }
    if (s_wifi_response_frame_len == 1 && byte != WIFI_RESPONSE_MAGIC_1) {
        reset_wifi_response_frame();
        if (byte == WIFI_RESPONSE_MAGIC_0) {
            s_wifi_response_frame[s_wifi_response_frame_len++] = byte;
        }
        return;
    }

    if (s_wifi_response_frame_len >= sizeof(s_wifi_response_frame)) {
        ESP_LOGE(TAG, "Wi-Fi response frame exceeded its buffer; discarding it");
        reset_wifi_response_frame();
        return;
    }
    s_wifi_response_frame[s_wifi_response_frame_len++] = byte;

    if (s_wifi_response_frame_len == WIFI_RESPONSE_HEADER_LEN) {
        uint32_t json_len = (uint32_t)s_wifi_response_frame[4] |
                            ((uint32_t)s_wifi_response_frame[5] << 8) |
                            ((uint32_t)s_wifi_response_frame[6] << 16) |
                            ((uint32_t)s_wifi_response_frame[7] << 24);
        if (json_len == 0 || json_len > MAX_WIFI_RESPONSE_JSON_LEN) {
            ESP_LOGE(TAG, "Invalid Wi-Fi response JSON length: %lu bytes",
                     (unsigned long)json_len);
            reset_wifi_response_frame();
            return;
        }
        s_wifi_response_expected_len = WIFI_RESPONSE_HEADER_LEN + json_len;
        ESP_LOGI(TAG, "Wi-Fi response frame received; JSON payload is %lu bytes",
                 (unsigned long)json_len);
    }

    if (s_wifi_response_expected_len != 0 &&
        s_wifi_response_frame_len == s_wifi_response_expected_len) {
        complete_wifi_response_frame();
    }
}

static void process_wifi_response_notification(const uint8_t *data,
                                              size_t data_len)
{
    for (size_t i = 0; i < data_len; i++) {
        process_wifi_response_byte(data[i]);
    }
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static bool is_separator(char value)
{
    return isspace((unsigned char)value) || value == ':' || value == '-';
}

static bool parse_command_payload(void)
{
    const char *cursor = CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX;

    while (*cursor != '\0') {
        int high;
        int low;

        while (is_separator(*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        high = hex_value(*cursor++);
        if (*cursor == '\0') {
            return false;
        }
        low = hex_value(*cursor++);
        if (high < 0 || low < 0 || s_command_len >= sizeof(s_command)) {
            return false;
        }
        s_command[s_command_len++] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void start_scan(void)
{
    esp_err_t error = esp_ble_gap_start_scanning(SCAN_DURATION_SECONDS);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start BLE scan: %s", esp_err_to_name(error));
    }
}

static void reject_connected_device(const char *reason)
{
    ESP_LOGI(TAG, "%s; disconnecting and resuming BLE scan", reason);
    esp_err_t error = esp_ble_gattc_close(s_gatt_if, s_conn_id);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not disconnect incompatible BLE device: %s",
                 esp_err_to_name(error));
        s_device_selected = false;
        start_scan();
    }
}

static uint16_t notification_config_for_handle(uint16_t handle)
{
    for (size_t i = 0; i < s_notification_count; i++) {
        if (s_notification_handles[i] == handle) {
            return s_notification_configs[i];
        }
    }
    return 0;
}

static void queue_characteristic_updates(
    const esp_gattc_char_elem_t *characteristic)
{
    uint16_t config = 0;

    if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0) {
        config |= 0x0001;
    }
    if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0) {
        config |= 0x0002;
    }
    if (config == 0) {
        return;
    }
    if (s_notification_count >= MAX_NOTIFICATION_CHARACTERISTICS) {
        ESP_LOGW(TAG, "Too many update characteristics; skipping handle %u",
                 characteristic->char_handle);
        return;
    }

    s_notification_handles[s_notification_count] = characteristic->char_handle;
    s_notification_configs[s_notification_count] = config;
    s_notification_count++;
}

static bool discover_characteristics(void)
{
    bool found_camera_service = false;
    bool has_wifi_control = false;
    bool has_wifi_response = false;

    if (s_discovered_service_overflow) {
        ESP_LOGE(TAG, "Too many GATT services to inspect safely");
        return false;
    }

    for (size_t service_index = 0;
         service_index < s_discovered_service_count; service_index++) {
        const discovered_service_t *service =
            &s_discovered_services[service_index];
        bool is_camera_service =
            service->uuid.len == ESP_UUID_LEN_16 &&
            service->uuid.uuid.uuid16 == CAMERA_SERVICE_UUID;
        esp_gattc_char_elem_t *characteristics;
        uint16_t count = 0;
        esp_err_t status = esp_ble_gattc_get_attr_count(
            s_gatt_if, s_conn_id, ESP_GATT_DB_CHARACTERISTIC,
            service->start_handle, service->end_handle,
            ESP_GATT_INVALID_HANDLE, &count);

        if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG,
                     "Could not count characteristics for GATT service "
                     "handles %u-%u: %d",
                     service->start_handle, service->end_handle, status);
            if (is_camera_service) {
                return false;
            }
            continue;
        }
        if (count == 0) {
            ESP_LOGI(TAG, "GATT service handles %u-%u has no characteristics",
                     service->start_handle, service->end_handle);
            if (is_camera_service) {
                found_camera_service = true;
            }
            continue;
        }

        characteristics = calloc(count, sizeof(*characteristics));
        if (characteristics == NULL) {
            ESP_LOGE(TAG, "Could not allocate memory for %u characteristics",
                     count);
            if (is_camera_service) {
                return false;
            }
            continue;
        }

        status = esp_ble_gattc_get_all_char(
            s_gatt_if, s_conn_id, service->start_handle, service->end_handle,
            characteristics, &count, 0);
        if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG,
                     "Could not read characteristics for GATT service "
                     "handles %u-%u: %d",
                     service->start_handle, service->end_handle, status);
            free(characteristics);
            if (is_camera_service) {
                return false;
            }
            continue;
        }

        log_gatt_uuid("service", &service->uuid);
        ESP_LOGI(TAG, "GATT service handles %u-%u has %u characteristic(s)",
                 service->start_handle, service->end_handle, count);
        if (is_camera_service) {
            found_camera_service = true;
            s_service_start_handle = service->start_handle;
            s_service_end_handle = service->end_handle;
        }

        for (uint16_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "Characteristic handle=%u properties=0x%02x",
                     characteristics[i].char_handle,
                     characteristics[i].properties);
            log_gatt_uuid("characteristic", &characteristics[i].uuid);

            if (!is_camera_service ||
                characteristics[i].uuid.len != ESP_UUID_LEN_16) {
                continue;
            }
            if (characteristics[i].uuid.uuid.uuid16 ==
                    WIFI_CONTROL_CHAR_UUID &&
                (characteristics[i].properties &
                 (ESP_GATT_CHAR_PROP_BIT_WRITE |
                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) != 0) {
                has_wifi_control = true;
            } else if (characteristics[i].uuid.uuid.uuid16 ==
                           WIFI_RESPONSE_CHAR_UUID &&
                       (characteristics[i].properties &
                        (ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                         ESP_GATT_CHAR_PROP_BIT_INDICATE)) != 0) {
                has_wifi_response = true;
                s_wifi_response_handle = characteristics[i].char_handle;
                queue_characteristic_updates(&characteristics[i]);
            }
        }
        free(characteristics);
    }

    if (!found_camera_service || !has_wifi_control || !has_wifi_response) {
        ESP_LOGI(TAG,
                 "BLE device is not compatible: service 0x%04x has no "
                 "writable 0x%04x and notify/indicate 0x%04x pair",
                 CAMERA_SERVICE_UUID, WIFI_CONTROL_CHAR_UUID,
                 WIFI_RESPONSE_CHAR_UUID);
        s_notification_count = 0;
        s_wifi_response_handle = 0;
        return false;
    }

    ESP_LOGI(TAG, "BLE device matches the expected camera GATT profile");
    return true;
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
    uint16_t config = notification_config_for_handle(characteristic_handle);
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
    if (s_notification_index >= s_notification_count) {
        find_control_characteristic();
        return;
    }

    uint16_t handle = s_notification_handles[s_notification_index];
    uint16_t config = s_notification_configs[s_notification_index];
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

static void select_ble_device(esp_ble_gap_cb_param_t *param)
{
    log_ble_device(param);
    memcpy(s_camera_address, param->scan_rst.bda, sizeof(s_camera_address));
    s_camera_address_type = param->scan_rst.ble_addr_type;
    s_device_selected = true;
    s_service_start_handle = 0;
    s_service_end_handle = 0;
    s_discovered_service_count = 0;
    s_discovered_service_overflow = false;
    s_notification_count = 0;
    s_notification_index = 0;
    s_wifi_response_handle = 0;
    reset_wifi_response_frame();
    ESP_LOGI(TAG, "Found BLE device; stopping scan to inspect its GATT profile");
    esp_ble_gap_stop_scanning();
}

static void find_control_characteristic(void)
{
    esp_bt_uuid_t char_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = WIFI_CONTROL_CHAR_UUID,
    };
    esp_gattc_char_elem_t characteristic;
    uint16_t count = 1;
    esp_err_t status;

    if (s_service_start_handle == 0 || s_service_end_handle == 0) {
        ESP_LOGE(TAG, "Camera service 0x%04x was not found", CAMERA_SERVICE_UUID);
        return;
    }

    status = esp_ble_gattc_get_char_by_uuid(s_gatt_if, s_conn_id,
                                            s_service_start_handle,
                                            s_service_end_handle, char_uuid,
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

    if (s_command_len == 0) {
        ESP_LOGW(TAG, "No payload configured; discovery complete, no write sent");
        return;
    }

    status = esp_ble_gattc_write_char(
        s_gatt_if, s_conn_id, characteristic.char_handle, (uint16_t)s_command_len,
        s_command,
        s_write_with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "GATT write request failed with status %d", status);
    } else {
        ESP_LOGI(TAG, "Wi-Fi enable payload submitted (%u bytes)",
                 (unsigned)s_command_len);
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
            ESP_LOGI(TAG, "Scanning for BLE devices to inspect service 0x%04x",
                     CAMERA_SERVICE_UUID);
        } else {
            ESP_LOGE(TAG, "BLE scan start failed: %d",
                     param->scan_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT &&
            !s_device_selected) {
            select_ble_device(param);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT &&
                   !s_device_selected) {
            ESP_LOGW(TAG, "No BLE device found; restarting scan");
            start_scan();
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (s_device_selected) {
            esp_err_t error = esp_ble_gattc_open(s_gatt_if, s_camera_address,
                                                 s_camera_address_type, true);
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Could not connect to BLE device: %s",
                         esp_err_to_name(error));
                s_device_selected = false;
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
            s_device_selected = false;
            start_scan();
            break;
        }
        s_conn_id = param->open.conn_id;
        ESP_LOGI(TAG, "Connected to BLE device; discovering GATT services");
        esp_err_t search_error = esp_ble_gattc_search_service(
            gattc_if, s_conn_id, NULL);
        if (search_error != ESP_OK) {
            ESP_LOGE(TAG, "Could not start GATT service discovery: %s",
                     esp_err_to_name(search_error));
            reject_connected_device("Service discovery could not be started");
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        log_gatt_uuid("service", &param->search_res.srvc_id.uuid);
        bool service_already_recorded = false;
        for (size_t i = 0; i < s_discovered_service_count; i++) {
            if (s_discovered_services[i].start_handle ==
                    param->search_res.start_handle &&
                s_discovered_services[i].end_handle ==
                    param->search_res.end_handle) {
                service_already_recorded = true;
                break;
            }
        }
        if (!service_already_recorded) {
            if (s_discovered_service_count < MAX_DISCOVERED_SERVICES) {
                discovered_service_t *service =
                    &s_discovered_services[s_discovered_service_count++];
                service->start_handle = param->search_res.start_handle;
                service->end_handle = param->search_res.end_handle;
                service->uuid = param->search_res.srvc_id.uuid;
            } else {
                s_discovered_service_overflow = true;
                ESP_LOGW(TAG, "More than %u GATT services found; "
                         "remaining services cannot be inspected",
                         MAX_DISCOVERED_SERVICES);
            }
        }
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == CAMERA_SERVICE_UUID) {
            s_service_start_handle = param->search_res.start_handle;
            s_service_end_handle = param->search_res.end_handle;
            ESP_LOGI(TAG, "Found camera service 0x%04x (handles %u-%u)",
                     CAMERA_SERVICE_UUID, s_service_start_handle,
                     s_service_end_handle);
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATT service discovery failed: %d",
                     param->search_cmpl.status);
            reject_connected_device("GATT service discovery failed");
        } else if (!discover_characteristics()) {
            reject_connected_device("GATT characteristics do not match");
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
        } else if (param->notify.handle == s_wifi_response_handle) {
            process_wifi_response_notification(param->notify.value,
                                               param->notify.value_len);
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
        ESP_LOGW(TAG, "BLE device disconnected; restarting discovery");
        s_device_selected = false;
        s_service_start_handle = 0;
        s_service_end_handle = 0;
        s_discovered_service_count = 0;
        s_discovered_service_overflow = false;
        s_notification_count = 0;
        s_notification_index = 0;
        s_wifi_response_handle = 0;
        reset_wifi_response_frame();
        start_scan();
        break;
    default:
        break;
    }
}

esp_err_t camera_ble_start(void)
{
    if (!parse_command_payload()) {
        ESP_LOGE(TAG, "Invalid Wi-Fi enable payload; expected up to %u hex bytes",
                 MAX_COMMAND_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting BLE PoC; identifying devices by GATT profile");
    if (s_command_len == 0) {
        ESP_LOGW(TAG, "Payload is empty; this run will not write to the camera");
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), TAG,
                        "Could not register BLE GAP callback");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(gattc_event_handler), TAG,
                        "Could not register BLE GATT client callback");
    return esp_ble_gattc_app_register(0);
}
