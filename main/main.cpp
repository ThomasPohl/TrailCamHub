/*
 * This module initializes diagnostics, persistent storage, the network stack
 * and the SD card, then starts the session controller, which runs a
 * camera/uplink cycle when one is requested, the serial console, which
 * accepts those requests and the hub settings, and, on the LilyGO T-Dongle,
 * the display and button as a further front end of the session controller.
 * The BLE stack is started by the camera BLE module when it is needed.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "camera_ble.h"
#include "camera_protocol.h"
#include "dongle_ui.h"
#include "hub_console.h"
#include "sd_card.h"
#include "sd_log.h"
#include "session_controller.h"
#include "system_diagnostics.h"
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
    const char *name = "idle";
    switch (status->phase) {
    case SESSION_PHASE_IDLE:
        if (status->last_result == SESSION_RESULT_CAMERA_SCANNED ||
            status->last_result == SESSION_RESULT_UPLINK_DONE) {
            phase = DONGLE_UI_FINISHED;
            name = "finished";
        }
        break;
    case SESSION_PHASE_CAMERA_WAKE:
        phase = DONGLE_UI_SEARCHING;
        name = "searching";
        break;
    case SESSION_PHASE_CAMERA_WIFI_JOIN:
        phase = DONGLE_UI_CONNECTING;
        name = "connecting";
        break;
    case SESSION_PHASE_CAMERA_SESSION:
        phase = DONGLE_UI_CONNECTED;
        name = "connected";
        break;
    // The display has no screen of its own for the backend sync yet.
    case SESSION_PHASE_UPLINK_WIFI_JOIN:
    case SESSION_PHASE_UPLINK:
        phase = DONGLE_UI_CONNECTED;
        name = "uploading";
        break;
    case SESSION_PHASE_CAMERA_RETRY_WAIT:
        phase = DONGLE_UI_RETRY_WAIT;
        name = "retry_wait";
        break;
    }
#if CONFIG_BOARD_TDONGLE
    if (s_display_ready) {
        dongle_ui_set_status(phase, status->camera_ssid,
                             status->retry_delay_ms);
    }
#else
    (void)phase;
#endif
#if CONFIG_WEB_UI_ENABLE
    if (s_web_ready) {
        web_ui_set_status(name, status->camera_ssid, status->retry_delay_ms);
    }
#else
    (void)name;
#endif
}

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
}
#endif

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

    error = diagnostics_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics are incomplete: %s", esp_err_to_name(error));
    }

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
    camera_protocol_set_progress_callback(on_media_progress);
#endif
    error = session_controller_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the session controller: %s",
                 esp_err_to_name(error));
    }

    error = hub_console_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the serial console: %s",
                 esp_err_to_name(error));
    }
}
