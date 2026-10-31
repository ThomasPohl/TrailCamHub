#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

/*
 * Notifications from the Wi-Fi station. They are called from the default
 * event loop task and must return quickly.
 */
typedef struct {
    /* The camera network was joined; `gateway` is the camera's address. */
    void (*on_connected)(const esp_ip4_addr_t *gateway);
    /* An established connection was lost unexpectedly. */
    void (*on_disconnected)(uint8_t reason);
    /* Joining failed after all retries. */
    void (*on_failed)(uint8_t reason);
} camera_wifi_callbacks_t;

/*
 * Initializes the Wi-Fi station; credentials are kept in RAM only. Requires
 * esp_netif_init() and the default event loop.
 */
esp_err_t camera_wifi_init(const camera_wifi_callbacks_t *callbacks);

/*
 * Joins the camera access point. Failed attempts are retried a few times
 * before on_failed is reported.
 */
esp_err_t camera_wifi_connect(const char *ssid, const char *password);

/*
 * Joins the home network for the backend uplink, with the same retries and
 * callbacks as camera_wifi_connect(). Only one network is joined at a time.
 */
esp_err_t camera_wifi_connect_home(const char *ssid, const char *password);

/* Leaves the current network without retrying. */
esp_err_t camera_wifi_disconnect(void);
