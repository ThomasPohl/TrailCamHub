#pragma once
#include "../../esp_stubs/esp_err.h"
extern "C" const char *esp_err_to_name(esp_err_t code);
#ifndef ESP_ERR_NOT_FOUND
// Distinct from the codes in esp_stubs (the value differs from ESP-IDF).
#define ESP_ERR_NOT_FOUND 0x10f
#endif
