/*
 * This module initializes diagnostics, persistent storage, the network stack
 * (with the Ethernet uplink on boards that have one) and the SD card, brings
 * the firmware of a Wi-Fi/BLE co-processor up to date, then starts the
 * session controller, which runs a camera/uplink cycle when one is requested,
 * the scheduler, which requests cycles on schedule and puts the hub to sleep
 * in between (autonomous profile), the serial console, which accepts those
 * requests and the hub settings, and,
 * on the LilyGO T-Dongle, the display and button as a further front end of
 * the session controller.
 * The BLE stack is started by the camera BLE module when it is needed.
 */

#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "camera_ble.h"
#include "camera_protocol.h"
#include "coprocessor_update.h"
#include "dongle_ui.h"
#include "ethernet_uplink.h"
#include "hub_console.h"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "scheduler.h"
#include "sd_card.h"
#include "sd_log.h"
#include "session_controller.h"
#include "system_diagnostics.h"
#include "time_sync.h"
#include "web_ui.h"

static const char *TAG = "trailcamhub";

#if CONFIG_BOARD_TDONGLE || CONFIG_WEB_UI_ENABLE
#if CONFIG_BOARD_TDONGLE
static bool s_display_ready;
#endif
#if CONFIG_WEB_UI_ENABLE
static bool s_web_ready;
#endif

// Passes the session status to the display and the web UI.
static void on_session_status(const session_status_t *status, void *context)
{
    (void)context;
    dongle_ui_phase_t phase = DONGLE_UI_IDLE;
    switch (status->front_end_phase) {
    case SESSION_FRONT_END_IDLE:
        phase = DONGLE_UI_IDLE;
        break;
    case SESSION_FRONT_END_SEARCHING:
        phase = DONGLE_UI_SEARCHING;
        break;
    case SESSION_FRONT_END_CONNECTING:
        phase = DONGLE_UI_CONNECTING;
        break;
    case SESSION_FRONT_END_CONNECTED:
    // The display has no screen of its own for the backend sync yet.
    case SESSION_FRONT_END_UPLOADING:
        phase = DONGLE_UI_CONNECTED;
        break;
    case SESSION_FRONT_END_RETRY_WAIT:
        phase = DONGLE_UI_RETRY_WAIT;
        break;
    case SESSION_FRONT_END_FINISHED:
        phase = DONGLE_UI_FINISHED;
        break;
    case SESSION_FRONT_END_LISTED:
        phase = DONGLE_UI_LISTED;
        break;
    case SESSION_FRONT_END_PARTIAL:
        phase = DONGLE_UI_PARTIAL;
        break;
    case SESSION_FRONT_END_NOT_FOUND:
        phase = DONGLE_UI_NOT_FOUND;
        break;
    case SESSION_FRONT_END_NO_CARD:
        phase = DONGLE_UI_NO_CARD;
        break;
    case SESSION_FRONT_END_UPLOAD_FAILED:
        phase = DONGLE_UI_UPLOAD_FAILED;
        break;
    case SESSION_FRONT_END_STOPPED:
        phase = DONGLE_UI_STOPPED;
        break;
    }
    // After a listing the front ends show the number of files on the
    // camera as the media total (nothing was saved).
    const bool listed = status->front_end_phase == SESSION_FRONT_END_LISTED;
#if CONFIG_BOARD_TDONGLE
    if (s_display_ready) {
        if (listed) {
            dongle_ui_set_media_progress(0, 0, status->listed_files);
        }
        dongle_ui_set_status(phase, status->camera_ssid,
                             status->retry_delay_ms);
    }
#else
    (void)phase;
#endif
#if CONFIG_WEB_UI_ENABLE
    if (s_web_ready) {
        if (listed) {
            web_ui_set_media_progress(0, 0, status->listed_files);
        }
        web_ui_set_status(status->front_end_phase_name, status->camera_ssid,
                          status->retry_delay_ms);
    }
#endif
    (void)listed;
}

#endif

// Passes the media download progress to every front end that shows it; the
// serial console exists on every board.
static void on_media_progress(uint32_t done, uint32_t stored, uint32_t total)
{
#if CONFIG_BOARD_TDONGLE
    if (s_display_ready) {
        dongle_ui_set_media_progress(done, stored, total);
    }
#endif
#if CONFIG_WEB_UI_ENABLE
    if (s_web_ready) {
        web_ui_set_media_progress(done, stored, total);
    }
#endif
    hub_console_set_media_progress(done, stored, total);
}

#if CONFIG_BOARD_TDONGLE || CONFIG_WEB_UI_ENABLE
static void on_search_request(void)
{
    session_controller_request_scan();
}

static void on_stop_request(void)
{
    session_controller_request_stop();
}
#endif

extern "C" void app_main(void)
{
    // Local time follows this zone (TZ as in the C library, set once).
    setenv("TZ", CONFIG_TIME_ZONE, 1);
    tzset();

#if CONFIG_SD_LOG_ENABLE
    // First, so that the boot messages are kept until the card is mounted.
    const bool sd_log_ready = sd_log_init() == ESP_OK;
#endif
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    // Restores the schedule after a wake-up from deep sleep.
    schedule::init();

    error = diagnostics_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics are incomplete: %s", esp_err_to_name(error));
    }
    // Firmware version and slot, and whether a new firmware still has to
    // confirm itself (it does after its first successful check-in).
    ota::log_boot_state();

#if CONFIG_SD_CARD_ENABLE
    // Media buffering is not implemented yet, so a missing card is not fatal.
    error = sd_card_mount();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without SD card");
    } else {
        camera_protocol_init_storage();
    }
#if CONFIG_SD_LOG_ENABLE
    if (sd_log_ready) {
        if (error != ESP_OK || sd_log_start() != ESP_OK) {
            sd_log_disable();
        }
    }
#endif
#endif

    // The network stack and the default event loop are shared by everything
    // that uses Wi-Fi or Ethernet (camera Wi-Fi, home Wi-Fi uplink, later an
    // access point or a wired uplink), so they are set up once here.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // The clock is needed for the times that are recorded and for the camera
    // login; it is set as soon as a network reaches an NTP server.
    time_sync_start();

#if CONFIG_COPROCESSOR_UPDATE_ENABLE
    // Restarts the application after an update; Wi-Fi and BLE may not work
    // with mismatched C6 firmware, but the rest of the firmware does.
    error = coprocessor_update_run();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing with the current C6 firmware");
    }
#endif

#if CONFIG_ETH_UPLINK_ENABLE
    // The backend sync still joins the home Wi-Fi; the wired link is not used
    // for it yet, so a missing Ethernet uplink is not fatal.
    error = ethernet_uplink_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without Ethernet uplink");
    }
#endif

    ESP_ERROR_CHECK(session_controller_init());
#if CONFIG_WEB_UI_ENABLE
    // Needs the Wi-Fi driver, which session_controller_init() started.
    static const web_ui_callbacks_t web_callbacks = {
        .on_search = on_search_request,
        .on_stop = on_stop_request,
    };
    s_web_ready = web_ui_start(&web_callbacks) == ESP_OK;
    if (s_web_ready) {
        // A running access point leaves the BLE connection setup on the
        // ESP32-C5 no air time once it has run for a while (camera
        // connections timed out), so it is closed for the few seconds until
        // the camera has answered.
        camera_ble_set_link_callback(web_ui_suspend_access_point);
    } else {
        ESP_LOGW(TAG, "Continuing without web UI");
    }
#endif
#if CONFIG_BOARD_TDONGLE
    static const dongle_ui_callbacks_t ui_callbacks = {
        .on_search = on_search_request,
        .on_stop = on_stop_request,
    };
    s_display_ready = dongle_ui_start(&ui_callbacks) == ESP_OK;
    if (!s_display_ready) {
        ESP_LOGW(TAG, "Continuing without display and button");
    }
#if CONFIG_WEB_UI_ENABLE
    if (s_display_ready && s_web_ready) {
        dongle_ui_set_web_access(web_ui_ssid(), web_ui_password(),
                                 web_ui_url());
    }
#endif
#endif
#if CONFIG_BOARD_TDONGLE || CONFIG_WEB_UI_ENABLE
    if (session_controller_add_status_listener(on_session_status, NULL) !=
        ESP_OK) {
        ESP_LOGW(TAG, "The display and web UI do not follow the session");
    }
#endif
    camera_protocol_set_progress_callback(on_media_progress);
    camera_protocol_set_list_callback(hub_console_list_event);
    error = session_controller_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the session controller: %s",
                 esp_err_to_name(error));
    } else {
        // Reads the cameras and checks in on schedule, and sleeps in between
        // with the autonomous profile.
        error = scheduler_start();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not start the scheduler: %s",
                     esp_err_to_name(error));
        }
    }

    error = hub_console_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the serial console: %s",
                 esp_err_to_name(error));
    }
}
