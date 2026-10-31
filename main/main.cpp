/*
 * This module initializes diagnostics, persistent storage, the network stack
 * (with the Ethernet uplink on boards that have one) and the SD card, brings
 * the firmware of a Wi-Fi/BLE co-processor up to date, then starts the
 * session controller, which runs a camera/uplink cycle when one is requested,
 * and the serial console, which accepts those requests and the hub settings.
 * The BLE stack is started by the camera BLE module when it is needed.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "camera_protocol.h"
#include "coprocessor_update.h"
#include "ethernet_uplink.h"
#include "hub_console.h"
#include "sd_card.h"
#include "sd_log.h"
#include "session_controller.h"
#include "system_diagnostics.h"

static const char *TAG = "trailcamhub";

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
