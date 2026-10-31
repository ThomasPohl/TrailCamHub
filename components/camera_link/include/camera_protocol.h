#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/*
 * True if the last camera_protocol_run() ended because all its work (media
 * list, thumbnails, downloads) was done, not because of a stop or an error.
 */
bool camera_protocol_work_completed(void);

/*
 * Progress of the media download in the running session: `done` of `total`
 * files handled, `stored` of them saved to the SD card. Called from the camera
 * session task when the download list is known and after each file; must
 * return quickly.
 */
typedef void (*camera_protocol_progress_cb_t)(uint32_t done, uint32_t stored,
                                              uint32_t total);

/*
 * Prepares writing media files to the SD card in the background (a task and
 * its buffer in PSRAM). Call once at boot, after the SD card is mounted;
 * without it files are written directly.
 */
void camera_protocol_init_storage(void);

/* Registers the progress callback; call before the first session. */
void camera_protocol_set_progress_callback(camera_protocol_progress_cb_t cb);
