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
 * Prepares the camera activation. Activation starts paused; call
 * camera_ble_resume() to start the BLE stack, scan for the camera, enable its
 * Wi-Fi and receive the credentials through `on_credentials`.
 */
esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials);

/*
 * Starts (or restarts) the camera activation: start the BLE stack if needed,
 * scan, connect, send the Wi-Fi-enable command. A still-open camera
 * connection is closed first so the command is sent again.
 */
void camera_ble_resume(void);

/*
 * Stops scanning and ignores further camera advertisements. With
 * CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION the camera connection is closed
 * and the BLE stack and controller are shut down to free their memory;
 * otherwise an existing camera connection is kept open.
 */
void camera_ble_pause(void);
