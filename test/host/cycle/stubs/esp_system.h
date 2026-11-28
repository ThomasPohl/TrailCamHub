#pragma once
// Host: the reset reason is set by the cycle driver (#446).
typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
} esp_reset_reason_t;
extern "C" esp_reset_reason_t esp_reset_reason(void);
