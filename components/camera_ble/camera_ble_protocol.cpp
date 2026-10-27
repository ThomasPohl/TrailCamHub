/*
 * This module converts the configured command from hexadecimal bytes and
 * assembles BLE notifications into framed Wi-Fi response JSON messages.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "ble_wifi_response.hpp"
#include "camera_ble_protocol.h"
#include "hex_payload.hpp"

#ifndef CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX
#error "main/Kconfig.projbuild was not processed"
#endif

using camera::ble::WifiResponseAssembler;

static const char *TAG = "trailcamhub_ble";
static uint8_t s_command[CAMERA_BLE_MAX_COMMAND_LEN];
static size_t s_command_len;
static WifiResponseAssembler s_wifi_response;
static camera_ble_credentials_cb_t s_on_credentials;

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
    bool credentials_valid =
        cJSON_IsNumber(ret) && ret->valuedouble == 0.0 &&
        cJSON_IsString(ssid) && ssid->valuestring != NULL &&
        cJSON_IsString(password) && password->valuestring != NULL;

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

    if (!credentials_valid) {
        ESP_LOGW(TAG, "Camera Wi-Fi credentials are missing or response "
                      "status is not successful");
    } else if (s_on_credentials != NULL) {
        esp_err_t error =
            s_on_credentials(ssid->valuestring, password->valuestring);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not connect to camera Wi-Fi: %s",
                     esp_err_to_name(error));
        }
    }

    cJSON_Delete(root);
}

static void process_wifi_response_byte(uint8_t byte)
{
    switch (s_wifi_response.push(byte)) {
    case WifiResponseAssembler::Event::header_received:
        ESP_LOGI(TAG,
                 "Wi-Fi response frame received; JSON payload is %lu bytes",
                 (unsigned long)s_wifi_response.json_size());
        break;
    case WifiResponseAssembler::Event::frame_complete:
        log_wifi_response_json(s_wifi_response.json(),
                               s_wifi_response.json_size(),
                               s_wifi_response.message_type());
        s_wifi_response.reset();
        break;
    case WifiResponseAssembler::Event::invalid_length:
        ESP_LOGE(TAG, "Invalid Wi-Fi response JSON length: %lu bytes",
                 (unsigned long)s_wifi_response.json_size());
        break;
    case WifiResponseAssembler::Event::overflow:
        ESP_LOGE(TAG,
                 "Wi-Fi response frame exceeded its buffer; discarding it");
        break;
    case WifiResponseAssembler::Event::none:
        break;
    }
}

bool camera_ble_protocol_init(camera_ble_credentials_cb_t on_credentials)
{
    s_on_credentials = on_credentials;
    s_wifi_response.reset();
    s_command_len = 0;
    return camera::parse_hex_payload(CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX,
                                     s_command, sizeof(s_command),
                                     &s_command_len);
}

uint8_t *camera_ble_protocol_command(size_t *length)
{
    *length = s_command_len;
    return s_command;
}

void camera_ble_protocol_reset_response(void)
{
    s_wifi_response.reset();
}

void camera_ble_protocol_process_notification(const uint8_t *data,
                                              size_t data_len)
{
    for (size_t i = 0; i < data_len; i++) {
        process_wifi_response_byte(data[i]);
    }
}
