#pragma once
#include <stddef.h>
#include <stdint.h>
// Host: log lines go to host_log_line() (collected by the tests).
typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;
extern "C" void host_log_line(int level, const char *tag, const char *format,
                              ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) host_log_line(ESP_LOG_ERROR, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log_line(ESP_LOG_WARN, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log_line(ESP_LOG_INFO, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log_line(ESP_LOG_DEBUG, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) host_log_line(ESP_LOG_VERBOSE, tag, __VA_ARGS__)
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buffer, len, level)                      \
    do {                                                                       \
        (void)(tag);                                                           \
        (void)(buffer);                                                        \
        (void)(len);                                                           \
        (void)(level);                                                         \
    } while (0)
