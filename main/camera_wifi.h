#pragma once

#include "esp_err.h"

esp_err_t camera_wifi_init(void);
esp_err_t camera_wifi_connect(const char *ssid, const char *password);
