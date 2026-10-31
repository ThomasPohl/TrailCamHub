/*
 * This module starts the log copy on the SD card, initializes diagnostics,
 * persistent storage, the SD card and the network stack (with the Ethernet
 * uplink on boards that have one), brings the firmware of a Wi-Fi/BLE
 * co-processor up to date, starts the web UI and the dongle display where
 * enabled, then starts the session controller, which runs a camera/uplink
 * cycle when one is requested, and the serial consoles, which accept those
 * requests and the hub settings. The BLE stack is started by the camera BLE
 * module when it is needed.
 */

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
#include "sd_card.h"
#include "sd_log.h"
#include "session_controller.h"
#include "system_diagnostics.h"
#include "web_ui.h"

static const char *TAG = "trailcamhub";

#if CONFIG_BOARD_TDONGLE
static bool s_display_ready;
#endif
#if CONFIG_WEB_UI_ENABLE
static bool s_web_ready;
#endif
static bool s_console_ready;

// Passes the workflow status to the display, the web UI and the console.
static void on_session_status(const session_status_info_t *info)
{
#if CONFIG_BOARD_TDONGLE
    if (s_display_ready) {
        dongle_ui_phase_t phase = DONGLE_UI_IDLE;
        switch (info->status) {
        case SESSION_STATUS_IDLE:
            break;
        case SESSION_STATUS_SEARCHING:
            phase = DONGLE_UI_SEARCHING;
            break;
        case SESSION_STATUS_CONNECTING:
            phase = DONGLE_UI_CONNECTING;
            break;
        case SESSION_STATUS_CONNECTED:
        // The display has no screen of its own for the backend sync yet.
        case SESSION_STATUS_UPLOADING:
            phase = DONGLE_UI_CONNECTED;
            break;
        case SESSION_STATUS_RETRY_WAIT:
            phase = DONGLE_UI_RETRY_WAIT;
            break;
        case SESSION_STATUS_FINISHED:
            phase = DONGLE_UI_FINISHED;
            break;
        }
        dongle_ui_set_status(phase, info->ssid, info->retry_delay_ms);
    }
#endif
    {
        const char *phase = "idle";
        switch (info->status) {
        case SESSION_STATUS_IDLE:
            break;
        case SESSION_STATUS_SEARCHING:
            phase = "searching";
            break;
        case SESSION_STATUS_CONNECTING:
            phase = "connecting";
            break;
        case SESSION_STATUS_CONNECTED:
            phase = "connected";
            break;
        case SESSION_STATUS_RETRY_WAIT:
            phase = "retry_wait";
            break;
        case SESSION_STATUS_UPLOADING:
            phase = "uploading";
            break;
        case SESSION_STATUS_FINISHED:
            phase = "finished";
            break;
        }
#if CONFIG_WEB_UI_ENABLE
        if (s_web_ready) {
            web_ui_set_status(phase, info->ssid, info->retry_delay_ms);
        }
#endif
        if (s_console_ready) {
            hub_console_set_status(phase, info->ssid, info->retry_delay_ms);
        }
    }
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
    if (s_console_ready) {
        hub_console_set_media_progress(done, stored, total);
    }
}

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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_COPROCESSOR_UPDATE_ENABLE
    // Restarts the application after an update; Wi-Fi and BLE may not work
    // with mismatched C6 firmware, but the rest of the firmware does.
    error = coprocessor_update_run();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing with the current C6 firmware");
    }
#endif

#if CONFIG_ETH_UPLINK_ENABLE
    // Uploads are not implemented yet, so a missing uplink is not fatal.
    error = ethernet_uplink_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without Ethernet uplink");
    }
#endif

    ESP_ERROR_CHECK(session_controller_init());
#if CONFIG_WEB_UI_ENABLE
    // Needs the Wi-Fi driver, which session_controller_init() started.
    static const web_ui_callbacks_t web_callbacks = {
        .on_search = session_controller_request_search,
        .on_stop = session_controller_request_stop,
    };
    s_web_ready = web_ui_start(&web_callbacks) == ESP_OK;
    if (!s_web_ready) {
        ESP_LOGW(TAG, "Continuing without web UI");
    }
#endif
#if CONFIG_WEB_UI_ENABLE
    // A running access point leaves the BLE connection setup on the ESP32-C5
    // no air time once it has run for a while (camera connections timed out),
    // so it is closed for the few seconds until the camera has answered.
    if (s_web_ready) {
        camera_ble_set_link_callback(web_ui_suspend_access_point);
    }
#endif
    // Scans, uploads, settings and diagnostics, for the web installer and
    // scripts, on the console port.
    s_console_ready = hub_console_start() == ESP_OK;
    if (!s_console_ready) {
        ESP_LOGW(TAG, "Continuing without serial console");
    }
#if CONFIG_BOARD_TDONGLE
    static const dongle_ui_callbacks_t ui_callbacks = {
        .on_search = session_controller_request_search,
        .on_stop = session_controller_request_stop,
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
    session_controller_set_status_callback(on_session_status);
    camera_protocol_set_progress_callback(on_media_progress);
    error = session_controller_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the session controller: %s",
                 esp_err_to_name(error));
    }
}
