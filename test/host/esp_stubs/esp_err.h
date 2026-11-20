#pragma once
// Host stand-in for ESP-IDF headers, so that the camera session (ESP-IDF
// code) compiles and runs in the host tests. Only what that code uses.
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x104
#define ESP_ERR_TIMEOUT 0x107
