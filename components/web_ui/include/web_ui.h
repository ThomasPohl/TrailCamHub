#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Web UI: on Wi-Fi boards the device opens its own access point next to the
 * camera station (AP+STA); with CONFIG_WEB_UI_OVER_ETHERNET (ESP32-P4) it
 * serves on the Ethernet interface at the address that DHCP assigned, and the
 * access point, its password and the channel handling do not exist. The page
 * starts and stops the camera search, follows the status and the live log,
 * and browses, views and downloads the files on the SD card. The component
 * knows nothing about the camera workflow: the application reports the status
 * and receives the button actions through callbacks.
 *
 * When the station joins the camera, the access point moves to the camera's
 * channel, so clients are disconnected briefly and usually reconnect on their
 * own. Over Ethernet nothing is disconnected.
 */

/* Called from the HTTP server task; they must return quickly. */
typedef struct {
    void (*on_search)(void);
    void (*on_stop)(void);
} web_ui_callbacks_t;

/*
 * Opens the access point and starts the HTTP server. Requires an initialized
 * and started Wi-Fi driver and NVS (for the generated password).
 */
esp_err_t web_ui_start(const web_ui_callbacks_t *callbacks);

/*
 * Closes the access point (true) and opens it again (false), for example
 * while a BLE connection is set up: on the ESP32-C5 a running access point
 * left the BLE connection setup no air time after it had run for a while.
 * Clients are disconnected and usually reconnect on their own. Safe to call
 * from any task; returns quickly.
 */
void web_ui_suspend_access_point(bool suspend);

/*
 * Access point name ("Ethernet" over Ethernet), password (empty over
 * Ethernet) and page address; valid after web_ui_start. Over Ethernet the
 * address is empty until DHCP has assigned one.
 */
const char *web_ui_ssid(void);
const char *web_ui_password(void);
const char *web_ui_url(void);

/*
 * Reports the camera workflow status shown on the page. `phase` is one of
 * "idle", "searching", "connecting", "connected", "retry_wait", "uploading",
 * "finished";
 * `ssid` may be empty. Safe to call from any task.
 */
void web_ui_set_status(const char *phase, const char *ssid,
                       uint32_t retry_delay_ms);

/*
 * Reports the media download progress: `done` of `total` files handled,
 * `stored` of them saved. Kept until the next search starts.
 */
void web_ui_set_media_progress(uint32_t done, uint32_t stored, uint32_t total);
