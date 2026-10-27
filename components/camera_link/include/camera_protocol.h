#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/*
 * Runs one camera session (discovery, PRE_LOGIN, ARTEMIS heartbeats) with the
 * camera at `gateway`. Blocks until the camera closes the session
 * (ESP_ERR_INVALID_STATE), an error occurs, or a stop is requested (ESP_OK).
 */
esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway);

/*
 * Asks a running session to end. Safe to call from any task; the session
 * returns within about one receive timeout (500 ms).
 */
void camera_protocol_request_stop(void);
