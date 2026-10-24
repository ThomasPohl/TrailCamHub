#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_nvs_flash.h"

#ifndef CONFIG_CAMERA_BLE_DEVICE_NAME
#define CONFIG_CAMERA_BLE_DEVICE_NAME "PR5000-WF"
#endif

#ifndef CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX
#define CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX ""
#endif

#define CAMERA_SERVICE_UUID 0x8801
#define WIFI_CONTROL_CHAR_UUID 0x0002
#define MAX_COMMAND_LEN 20
#define SCAN_DURATION_SECONDS 30

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
static uint16_t s_service_start_handle;
static uint16_t s_service_end_handle;
static uint8_t s_command[MAX_COMMAND_LEN];
static size_t s_command_len;
static esp_bd_addr_t s_camera_address;
static esp_ble_addr_type_t s_camera_address_type;
static bool s_camera_selected;
static bool s_write_with_response;

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

static void select_camera(const esp_ble_gap_cb_param_t *param)
{
    const uint8_t *name;
    uint8_t name_len = 0;
    size_t configured_name_len = strlen(CONFIG_CAMERA_BLE_DEVICE_NAME);

    name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                    ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL) {
        name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                        ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name == NULL || name_len != configured_name_len ||
        memcmp(name, CONFIG_CAMERA_BLE_DEVICE_NAME, configured_name_len) != 0) {
        return;
    }

    memcpy(s_camera_address, param->scan_rst.bda, sizeof(s_camera_address));
    s_camera_address_type = param->scan_rst.ble_addr_type;
    s_camera_selected = true;
    ESP_LOGI(TAG, "Found camera %s; stopping scan", CONFIG_CAMERA_BLE_DEVICE_NAME);
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
                                            s_service_end_handle, &char_uuid,
                                            &characteristic, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGE(TAG, "Writable characteristic 0x%04x was not found",
                 WIFI_CONTROL_CHAR_UUID);
        return;
    }

    ESP_LOGI(TAG, "Found characteristic 0x%04x (properties 0x%02x)",
             WIFI_CONTROL_CHAR_UUID, characteristic.properties);
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
        if (param->scan_start.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Scanning for camera %s", CONFIG_CAMERA_BLE_DEVICE_NAME);
        } else {
            ESP_LOGE(TAG, "BLE scan start failed: %d", param->scan_start.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT &&
            !s_camera_selected) {
            select_camera(param);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT &&
                   !s_camera_selected) {
            ESP_LOGW(TAG, "Camera not found; restarting scan");
            start_scan();
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (s_camera_selected) {
            esp_err_t error = esp_ble_gattc_open(s_gatt_if, s_camera_address,
                                                 s_camera_address_type, true);
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Could not connect to camera: %s",
                         esp_err_to_name(error));
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
            ESP_LOGE(TAG, "Camera connection failed: %d", param->open.status);
            break;
        }
        s_conn_id = param->open.conn_id;
        ESP_LOGI(TAG, "Connected; discovering GATT services");
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.id.uuid.uuid.uuid16 == CAMERA_SERVICE_UUID) {
            s_service_start_handle = param->search_res.start_handle;
            s_service_end_handle = param->search_res.end_handle;
            ESP_LOGI(TAG, "Found camera service 0x%04x", CAMERA_SERVICE_UUID);
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status == ESP_GATT_OK) {
            find_control_characteristic();
        } else {
            ESP_LOGE(TAG, "GATT service discovery failed: %d",
                     param->search_cmpl.status);
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
        ESP_LOGW(TAG, "Camera disconnected; restarting discovery");
        s_camera_selected = false;
        s_service_start_handle = 0;
        s_service_end_handle = 0;
        start_scan();
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t error = esp_nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(esp_nvs_flash_erase());
        error = esp_nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    if (!parse_command_payload()) {
        ESP_LOGE(TAG, "Invalid Wi-Fi enable payload; expected up to %u hex bytes",
                 MAX_COMMAND_LEN);
        return;
    }

    ESP_LOGI(TAG, "Starting BLE PoC for %s", CONFIG_CAMERA_BLE_DEVICE_NAME);
    if (s_command_len == 0) {
        ESP_LOGW(TAG, "Payload is empty; this run will not write to the camera");
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));
}