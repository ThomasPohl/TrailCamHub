#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/*
 * Runs one camera session after the station obtained an IP address. Called
 * from the camera Wi-Fi worker task; `gateway` is the camera's address.
 */
typedef esp_err_t (*camera_wifi_session_fn_t)(const esp_ip4_addr_t *gateway);

/*
 * Initializes the Wi-Fi station (credentials kept in RAM only) and starts the
 * worker task that calls `run_session` each time the camera network is
 * joined.
 */
esp_err_t camera_wifi_init(camera_wifi_session_fn_t run_session);

/* Joins the camera access point with the credentials returned over BLE. */
esp_err_t camera_wifi_connect(const char *ssid, const char *password);
