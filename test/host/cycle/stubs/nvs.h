#pragma once
// Host: NVS as a map in memory (#446). It survives a simulated restart or
// deep sleep; host_nvs::erase_all() is a fresh flash.
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_READ_ONLY (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

extern "C" {
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_get_i64(nvs_handle_t handle, const char *key, int64_t *out);
esp_err_t nvs_set_i64(nvs_handle_t handle, const char *key, int64_t value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
}
