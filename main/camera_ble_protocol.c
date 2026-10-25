#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"

#include "camera_ble_protocol.h"

#ifndef CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX
#define CONFIG_CAMERA_WIFI_ENABLE_PAYLOAD_HEX "42545F4B45595F4F4E"
#endif

#define WIFI_RESPONSE_HEADER_LEN 8
#define MAX_WIFI_RESPONSE_JSON_LEN 512
#define MAX_WIFI_RESPONSE_FRAME_LEN \
    (WIFI_RESPONSE_HEADER_LEN + MAX_WIFI_RESPONSE_JSON_LEN)
#define WIFI_RESPONSE_MAGIC_0 0x13
#define WIFI_RESPONSE_MAGIC_1 0x57

static const char *TAG = "trailcamhub_ble";
static uint8_t s_command[CAMERA_BLE_MAX_COMMAND_LEN];
static size_t s_command_len;
static uint8_t s_wifi_response_frame[MAX_WIFI_RESPONSE_FRAME_LEN];
static size_t s_wifi_response_frame_len;
static size_t s_wifi_response_expected_len;

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

    s_command_len = 0;
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

bool camera_ble_protocol_init(void)
{
    reset_wifi_response_frame();
    return parse_command_payload();
}

uint8_t *camera_ble_protocol_command(size_t *length)
{
    *length = s_command_len;
    return s_command;
}

void camera_ble_protocol_reset_response(void)
{
    reset_wifi_response_frame();
}

void camera_ble_protocol_process_notification(const uint8_t *data,
                                              size_t data_len)
{
    for (size_t i = 0; i < data_len; i++) {
        process_wifi_response_byte(data[i]);
    }
}
