#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * The session controller runs TrailCamHub cycles in its own task. It is idle
 * until a cycle is requested; a scan activates the camera over BLE, joins its
 * Wi-Fi, runs the camera session and then, if the uplink settings are
 * complete, syncs with the backend over the home Wi-Fi. Camera failures are
 * retried a bounded number of times per scan. Progress is printed as
 * "TCH EVENT ..." lines on the console.
 */

/* Phase of the cycle, for a display, the web UI or a console. */
typedef enum {
    SESSION_STATUS_IDLE,
    SESSION_STATUS_SEARCHING,
    SESSION_STATUS_CONNECTING,
    SESSION_STATUS_CONNECTED,
    SESSION_STATUS_RETRY_WAIT,
    /* Joining the home Wi-Fi or syncing with the backend. */
    SESSION_STATUS_UPLOADING,
    /* Idle after a cycle that did its work. */
    SESSION_STATUS_FINISHED,
} session_status_t;

typedef struct {
    session_status_t status;
    /* Camera Wi-Fi name once the camera has reported it, otherwise empty. */
    char ssid[33];
    /* Delay until the next attempt for SESSION_STATUS_RETRY_WAIT. */
    uint32_t retry_delay_ms;
} session_status_info_t;

/* Called from the controller task after each state change; must return
 * quickly. */
typedef void (*session_status_cb_t)(const session_status_info_t *info);

/* Registers the status callback; call before session_controller_start(). */
void session_controller_set_status_callback(session_status_cb_t callback);

/* Creates the controller's queues and timers and initializes Wi-Fi. */
esp_err_t session_controller_init(void);

/* Prepares camera BLE and starts the controller tasks. */
esp_err_t session_controller_start(void);

/*
 * Requests a scan (camera, then backend sync if configured). Returns
 * ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_scan(void);

/*
 * Requests a backend sync of the data already on the SD card. Returns
 * ESP_ERR_NOT_FOUND if the uplink settings are incomplete and
 * ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_upload(void);

/*
 * For a button or a web page: requests a scan, or does nothing while a
 * cycle is running.
 */
void session_controller_request_search(void);

/*
 * Stops the running cycle (BLE activation, camera Wi-Fi, session, home Wi-Fi
 * join) and returns to idle; a running backend sync is completed.
 */
void session_controller_request_stop(void);

/* Name of the current controller state, e.g. "IDLE" or "SESSION". */
const char *session_controller_state_name(void);
