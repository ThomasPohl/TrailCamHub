#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

/*
 * Runs one camera session (discovery, PRE_LOGIN, ARTEMIS heartbeats) with the
 * camera at `gateway`. Blocks until the camera closes the session
 * (ESP_ERR_INVALID_STATE), an error occurs, the work is done, a listing is
 * done or a stop is requested (ESP_OK), the work cannot be done, e.g. without
 * an SD card in the camera (ESP_ERR_INVALID_RESPONSE, see
 * camera_protocol_media_list_error()), or the camera made no progress for
 * CAMERA_SESSION_IDLE_TIMEOUT_S (ESP_ERR_TIMEOUT).
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
 * True if in the last camera_protocol_run() the camera reported an error for
 * its media list (getMediaListRet != 0), as it does without an SD card.
 */
bool camera_protocol_media_list_error(void);

/*
 * Thumbnails and files stored on the SD card in the last
 * camera_protocol_run(), also when it ended with an error.
 */
uint32_t camera_protocol_stored_items(void);

/*
 * Battery level in percent that the camera reported in the last session
 * (device info, cmdId 512); -1 if no camera has reported one since boot.
 */
int camera_protocol_last_battery_percent(void);

/*
 * Progress of the media download in the running session: `done` of `total`
 * files handled, `stored` of them saved to the SD card. Called from the camera
 * session task when the download list is known and after each file; must
 * return quickly.
 */
typedef void (*camera_protocol_progress_cb_t)(uint32_t done, uint32_t stored,
                                              uint32_t total);

/*
 * Makes the following sessions only list the camera's media, or return to
 * normal ones: they read all pages of the media list, report them through the
 * list callback and end. No thumbnail or file is fetched, and the catalog on
 * the SD card is not touched. Set it before camera_protocol_run().
 */
void camera_protocol_set_list_only(bool list_only);

/*
 * Receives the lines of a listing: "media <entry>" per file (see
 * format_media_item()), then "list done files=<n> pages=<n>",
 * "list truncated files=<n> pages=<n>" or "list failed <reason>". Without the
 * final line the session ended early. Called from the camera session task;
 * must return quickly.
 */
typedef void (*camera_protocol_list_cb_t)(const char *line);
void camera_protocol_set_list_callback(camera_protocol_list_cb_t cb);

/*
 * Prepares writing media files to the SD card in the background (a task and
 * its buffer in PSRAM). Call once at boot, after the SD card is mounted;
 * without it files are written directly.
 */
void camera_protocol_init_storage(void);

/* Registers the progress callback; call before the first session. */
void camera_protocol_set_progress_callback(camera_protocol_progress_cb_t cb);
