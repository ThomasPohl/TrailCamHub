/*
 * This module initializes diagnostics, persistent storage, the SD card and
 * the Bluetooth LE stack, then starts the application's camera BLE workflow.
 */

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "camera_ble.h"
#include "camera_wifi.h"
#include "sd_card.h"
#include "system_diagnostics.h"

static const char *TAG = "trailcamhub";

extern "C" void app_main(void)
{
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
    }
#endif

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(camera_wifi_init());
    diagnostics_log_memory("Wi-Fi initialized");
    esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    error = camera_ble_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start camera BLE: %s", esp_err_to_name(error));
    }
    diagnostics_log_memory("BLE started");
}
