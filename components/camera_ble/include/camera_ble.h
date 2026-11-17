#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * camera_ble_resume() to start the BLE stack, scan for a camera (the first
 * one not done in this scan), enable its Wi-Fi and receive the credentials
 * through `on_credentials`.
 */
esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials);

/*
 * Called with true when a connection to the camera is being set up, and with
 * false once the BLE stack has been released (or, without
 * CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION, once the connection attempt or
 * connection has ended). Further attempts in between are not reported. For
 * example, the Wi-Fi access point is closed in between, because on the
 * ESP32-C5 it left the BLE connection setup no air time. Called from the
 * Bluetooth task or from the task that calls camera_ble_pause(); must return
 * quickly.
 */
typedef void (*camera_ble_link_cb_t)(bool active);

/* Registers the link callback; call before camera_ble_resume(). */
void camera_ble_set_link_callback(camera_ble_link_cb_t callback);

/*
 * Starts (or restarts) the camera activation: start the BLE stack if needed,
 * scan, connect, send the Wi-Fi-enable command. A still-open camera
 * connection is closed first so the command is sent again.
 */
void camera_ble_resume(void);

/*
 * A scan can read several cameras: after one is done, camera_ble_mark_done()
 * makes the following activations skip it, so camera_ble_resume() reaches
 * the next one. camera_ble_clear_done() starts a new scan with no camera
 * done. Up to 8 cameras are remembered.
 */
void camera_ble_clear_done(void);
/* Marks the camera of the last connection as done for this scan. */
void camera_ble_mark_done(void);

/*
 * Copies the advertised name of the camera of the last connection, e.g.
 * "TC100_ED3B" ("unknown" if none).
 */
void camera_ble_last_name(char *out, size_t size);

/*
 * BLE address of the camera of the last connection, "AA:BB:CC:DD:EE:FF", or
 * "" if there is none (#296). Take it before camera_ble_mark_done().
 */
void camera_ble_last_address(char *out, size_t size);

/*
 * A camera heard advertising in a scan, read or not (#296). The scans only
 * listen; nothing connects to a camera for this list.
 */
#define CAMERA_BLE_MAX_SEEN 16
typedef struct {
    char address[18]; /* "AA:BB:CC:DD:EE:FF" */
    char name[33];    /* advertised name; "" if none was heard */
    int rssi;         /* dBm */
    int64_t seen_at;  /* Unix seconds of the hub clock; may be unset */
} camera_ble_seen_t;

/*
 * Copies the cameras heard since the last call (at most `max`) and empties
 * the list. Returns how many were copied.
 */
size_t camera_ble_take_seen(camera_ble_seen_t *out, size_t max);

/*
 * The cameras the hub may read (#299), as "AA:BB:CC:DD:EE:FF"; copied. Count
 * 0 = every camera (the default). Others are still heard for the list above.
 * Returns how many addresses were kept (at most 32).
 */
size_t camera_ble_set_allowed(const char *const *addresses, size_t count);

/*
 * Stops scanning and ignores further camera advertisements. With
 * CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION the camera connection is closed
 * and the BLE stack and controller are shut down to free their memory;
 * otherwise an existing camera connection is kept open.
 */
void camera_ble_pause(void);
