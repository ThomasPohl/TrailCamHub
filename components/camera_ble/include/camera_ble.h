#pragma once

#include "esp_err.h"

/*
 * Called from the Bluetooth task when the camera has returned Wi-Fi
 * credentials. The strings are only valid during the call, and the callback
 * must return quickly.
 */
typedef esp_err_t (*camera_ble_credentials_cb_t)(const char *ssid,
                                                 const char *password);

/*
 * Registers the BLE GATT client. Activation starts paused; call
 * camera_ble_resume() to scan for the camera, enable its Wi-Fi and receive
 * the credentials through `on_credentials`. The Bluetooth controller and
 * Bluedroid must already be enabled.
 */
esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials);

/*
 * Starts (or restarts) the camera activation: scan, connect, send the
 * Wi-Fi-enable command. A still-open camera connection is closed first so
 * the command is sent again.
 */
void camera_ble_resume(void);

/*
 * Stops scanning and ignores further camera advertisements. An existing
 * camera connection is kept open.
 */
void camera_ble_pause(void);
