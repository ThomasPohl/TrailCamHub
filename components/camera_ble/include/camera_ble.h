#pragma once

#include "esp_err.h"

/*
 * Called from the Bluetooth task when the camera has returned Wi-Fi
 * credentials. The strings are only valid during the call.
 */
typedef esp_err_t (*camera_ble_credentials_cb_t)(const char *ssid,
                                                 const char *password);

/*
 * Starts scanning for the camera, enables its Wi-Fi over BLE and reports the
 * returned credentials to `on_credentials`. The Bluetooth controller and
 * Bluedroid must already be enabled.
 */
esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials);
