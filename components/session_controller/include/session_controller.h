#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The session controller runs TrailCamHub cycles in its own task. It is idle
 * until a cycle is requested; a scan activates a camera over BLE, joins its
 * Wi-Fi and runs the camera session, then does the same with every further
 * camera that answers (up to CONFIG_SESSION_MAX_CAMERAS), and finally, if the
 * uplink settings are complete, syncs with the backend over the home Wi-Fi.
 * Camera failures are retried a bounded number of times per camera; a camera
 * that still fails is skipped.
 *
 * The controller does not know who drives it. Front ends (serial console,
 * display and button, a local web UI, the scheduler) request cycles with
 * the functions below and follow the progress through status listeners or
 * session_controller_get_status().
 */

typedef enum {
    SESSION_PHASE_IDLE,
    /* Waking the camera over BLE and waiting for its Wi-Fi credentials. */
    SESSION_PHASE_CAMERA_WAKE,
    SESSION_PHASE_CAMERA_WIFI_JOIN,
    SESSION_PHASE_CAMERA_SESSION,
    /* A camera attempt failed; the next one starts after retry_delay_ms. */
    SESSION_PHASE_CAMERA_RETRY_WAIT,
    SESSION_PHASE_UPLINK_WIFI_JOIN,
    SESSION_PHASE_UPLINK,
} session_phase_t;

typedef enum {
    SESSION_RESULT_NONE,
    /* Camera session done; no uplink configured. */
    SESSION_RESULT_CAMERA_SCANNED,
    /* Listing of the camera's media done (list_only); never synced. */
    SESSION_RESULT_CAMERA_LISTED,
    /* Camera session (unless upload only) and backend sync done; see
     * camera_partial and camera_no_card for how the camera session ended. */
    SESSION_RESULT_UPLINK_DONE,
    SESSION_RESULT_CAMERA_UNREACHABLE,
    /* The camera reported an error for its media list (no SD card) and no
     * sync followed (none configured, or a listing). */
    SESSION_RESULT_CAMERA_NO_CARD,
    SESSION_RESULT_UPLINK_UNREACHABLE,
    /* Ended early by session_controller_request_stop(). */
    SESSION_RESULT_STOPPED,
} session_result_t;

/*
 * What the front ends show: the console ("phase ..."), the web UI and the
 * dongle display. Derived from the phase and, when idle, the last result.
 */
typedef enum {
    SESSION_FRONT_END_IDLE,
    SESSION_FRONT_END_SEARCHING,
    SESSION_FRONT_END_CONNECTING,
    SESSION_FRONT_END_CONNECTED,
    SESSION_FRONT_END_RETRY_WAIT,
    /* Joining the home Wi-Fi or syncing with the backend. */
    SESSION_FRONT_END_UPLOADING,
    /* Idle after a cycle with the result CAMERA_SCANNED or UPLINK_DONE (and
     * neither camera_partial nor camera_no_card). */
    SESSION_FRONT_END_FINISHED,
    /* Idle after a listing (CAMERA_LISTED); see listed_files. */
    SESSION_FRONT_END_LISTED,
    /* The same, but the camera session ended with an error after storing
     * some media (camera_partial). */
    SESSION_FRONT_END_PARTIAL,
    /* Idle after CAMERA_UNREACHABLE, CAMERA_NO_CARD (or camera_no_card with
     * a sync), UPLINK_UNREACHABLE or STOPPED; kept until the next cycle
     * starts. */
    SESSION_FRONT_END_NOT_FOUND,
    SESSION_FRONT_END_NO_CARD,
    SESSION_FRONT_END_UPLOAD_FAILED,
    SESSION_FRONT_END_STOPPED,
} session_front_end_phase_t;

/* How one camera of a scan ended. */
typedef enum {
    SESSION_CAMERA_READ,
    /* Its session stored some media before an error. */
    SESSION_CAMERA_PARTIAL,
    SESSION_CAMERA_NO_CARD,
    /* Found over BLE, but not read after all attempts. */
    SESSION_CAMERA_FAILED,
} session_camera_result_t;

/* Cameras per scan whose results the status keeps (Kconfig maximum). */
#define SESSION_MAX_CAMERA_RESULTS 4

typedef struct {
    session_camera_result_t result;
    /* Stable name: "read", "partial", "no-card" or "failed". */
    const char *result_name;
    /* Advertised BLE name, e.g. "TC100_ED3B" ("unknown" if none). */
    char name[32];
    /*
     * The camera's id as on the SD card and in the backend (its sanitized
     * devId); empty if the session did not get that far.
     */
    char cam_id[48];
    /* Battery in percent reported by the camera; -1 if unknown. */
    int bat_percent;
} session_camera_t;

typedef struct {
    session_phase_t phase;
    /* Stable name of the phase as used on the console, e.g. "IDLE". */
    const char *phase_name;
    /* A stop was requested and the camera session is closing. */
    bool stopping;
    /* Attempt for the current camera (1-based; 0 when idle). */
    uint32_t attempt;
    uint32_t max_attempts;
    /*
     * Cameras done in this cycle (read, without card or skipped), and of
     * those the ones read; the first SESSION_MAX_CAMERA_RESULTS are listed in
     * `cameras`. camera_finished is true only on the update on which a camera
     * ended: it is the last one listed. Kept after the cycle ends and cleared
     * when the next cycle starts.
     */
    uint32_t cameras_done;
    uint32_t cameras_read;
    uint32_t max_cameras;
    session_camera_t cameras[SESSION_MAX_CAMERA_RESULTS];
    bool camera_finished;
    /* Looking for a further camera after one was done. */
    bool searching_next_camera;
    /* Delay until the next attempt in SESSION_PHASE_CAMERA_RETRY_WAIT. */
    uint32_t retry_delay_ms;
    /*
     * Camera Wi-Fi name once the camera reported it in this cycle. Kept after
     * the cycle ends and cleared when the next cycle starts; empty for an
     * upload-only cycle.
     */
    char camera_ssid[33];
    /*
     * A camera was read in this cycle: its session did all its work, or
     * stored some media before an error (camera_partial: one of them did).
     * The backend sync of this cycle succeeded. Kept after the cycle ends and
     * cleared when the next cycle starts.
     */
    bool camera_ok;
    bool camera_partial;
    /*
     * A camera reported no SD card (its media list failed). The cycle still
     * syncs, if configured, so its result may be UPLINK_DONE; the front-end
     * phase is NO_CARD when no other camera was read. Kept like camera_ok.
     */
    bool camera_no_card;
    bool sync_ok;
    /*
     * The cycle only lists the camera's media
     * (session_controller_request_list()). Kept after the cycle ends and
     * cleared when the next cycle starts.
     */
    bool list_only;
    /*
     * The cycle is a short session that only runs the queued camera actions
     * (session_controller_request_actions()). Kept like list_only.
     */
    bool actions_only;
    /* Files reported by the listing of this cycle; kept like list_only. */
    uint32_t listed_files;
    /* True only on the update that ends a cycle. */
    bool cycle_finished;
    /* Result of the last finished cycle; NONE while a new one runs. */
    session_result_t last_result;
    /* Stable name of last_result, e.g. "uplink-done" or "none". */
    const char *last_result_name;
    session_front_end_phase_t front_end_phase;
    /*
     * Stable name of front_end_phase for the console and the web UI JSON:
     * "idle", "searching", "connecting", "connected", "retry_wait",
     * "uploading", "finished", "listed", "partial", "not_found", "no_card",
     * "upload_failed" or "stopped".
     */
    const char *front_end_phase_name;
} session_status_t;

/*
 * Called from the controller task after each change; must return quickly. It
 * may call the request functions, which only queue the request.
 */
typedef void (*session_status_listener_t)(const session_status_t *status,
                                          void *context);

/* Maximum number of status listeners. */
#define SESSION_CONTROLLER_MAX_LISTENERS 4

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
 * Requests a listing of the media on the camera (see
 * camera_protocol_set_list_only()): a camera part without any fetching and
 * without a backend sync, which does not count as a read of the camera for the
 * schedule. Returns ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_list(void);

/*
 * Requests a short camera session that runs the queued actions (see
 * camera_actions.hpp) and nothing else: no thumbnails, files or backend sync.
 * Actions queued while no such session is requested run at the next scan
 * instead. Returns ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_actions(void);

/*
 * Requests a backend sync of the data already on the SD card. Returns
 * ESP_ERR_NOT_FOUND if the uplink settings are incomplete and
 * ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_upload(void);

/*
 * Requests the end of the running cycle: the camera part stops (a running
 * camera session closes cleanly first) and the cycle ends without the backend
 * sync. A sync that is already running is not interrupted. Returns
 * ESP_ERR_INVALID_STATE when idle or during the sync.
 */
esp_err_t session_controller_request_stop(void);

/*
 * Registers a listener for status changes; use session_controller_get_status()
 * for the status at registration. Returns ESP_ERR_NO_MEM when all slots are
 * taken.
 */
esp_err_t
session_controller_add_status_listener(session_status_listener_t listener,
                                       void *context);

/* Copies the current status (for polling front ends). */
void session_controller_get_status(session_status_t *out);

/* Name of the current controller state, e.g. "IDLE" or "SESSION". */
const char *session_controller_state_name(void);
