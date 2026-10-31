#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Status display and search button of the LilyGO T-Dongle-S3 and T-Dongle-C5.
 * The component knows nothing about the camera workflow: the application
 * reports the phase and receives the button actions through callbacks.
 */

typedef enum {
    DONGLE_UI_IDLE,
    DONGLE_UI_SEARCHING,
    DONGLE_UI_CONNECTING,
    DONGLE_UI_CONNECTED,
    DONGLE_UI_RETRY_WAIT,
    /* Idle after a session that did all its work. */
    DONGLE_UI_FINISHED,
} dongle_ui_phase_t;

/* Called from the UI task; they must return quickly. */
typedef struct {
    /* Short button press. */
    void (*on_search)(void);
    /* Long button press. */
    void (*on_stop)(void);
} dongle_ui_callbacks_t;

/* Initializes the display and the button and starts the UI task. */
esp_err_t dongle_ui_start(const dongle_ui_callbacks_t *callbacks);

/*
 * Updates the screen. `ssid` may be empty; `retry_delay_ms` is the time until
 * the next automatic attempt and only used for DONGLE_UI_RETRY_WAIT. Safe to
 * call from any task.
 */
void dongle_ui_set_status(dongle_ui_phase_t phase, const char *ssid,
                          uint32_t retry_delay_ms);

/*
 * Shows the media download progress: while connected, file `done` + 1 of
 * `total` is being saved; when finished, `stored` of `total` were saved.
 * Cleared when a new search starts. Safe to call from any task.
 */
void dongle_ui_set_media_progress(uint32_t done, uint32_t stored,
                                  uint32_t total);

/* Shows how to reach the web UI on the idle screen. */
void dongle_ui_set_web_access(const char *ssid, const char *key,
                              const char *url);
