// Camera mock (#122, stage 1): an ESP32-WROOM that stands in for a TC100 on
// the bench. BLE wake-up → Wi-Fi AP → discovery, PRE_LOGIN, login, device
// info, media list, heartbeat, CLOSE. See README.md.

#include <cstdlib>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_frames.hpp"
#include "capture_fixtures.hpp"
#include "mock_ap.hpp"
#include "mock_ble.h"
#include "mock_console.hpp"
#include "mock_udp.hpp"

namespace
{

const char *TAG = "camera_mock";
// Notification size of the camera in the btsnoop capture (ATT MTU 23).
constexpr std::size_t kNotificationSize = 20;

QueueHandle_t s_wake_queue;

// NimBLE host task: only queue the work, never block here.
void on_ble_write(uint16_t conn_handle, const uint8_t *data, size_t size)
{
    if (!camera_mock::ble::is_wake_command(data, size)) {
        ESP_LOGW(TAG, "Write of %u bytes to 0x0002 is no wake-up; ignored",
                 static_cast<unsigned>(size));
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, size, ESP_LOG_WARN);
        return;
    }
    ESP_LOGI(TAG, "Wake-up written (%u bytes)", static_cast<unsigned>(size));
    if (xQueueSend(s_wake_queue, &conn_handle, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Wake-up already pending");
    }
}

// Turns the Wi-Fi on and answers with the credentials, as the camera does.
void wake_task(void *arg)
{
    (void)arg;
    uint16_t conn_handle = 0;
    while (true) {
        xQueueReceive(s_wake_queue, &conn_handle, portMAX_DELAY);
        if (!mock_ap::start()) {
            ESP_LOGE(TAG, "Access point did not start; no credentials sent");
            continue;
        }
        const std::vector<std::uint8_t> frame =
            camera_mock::ble::credentials_frame(
                mock_ap::ssid(), mock_ap::bssid(), mock_ap::password());
        for (const auto &chunk :
             camera_mock::ble::split_notifications(frame, kNotificationSize)) {
            if (mock_ble_notify(conn_handle, chunk.data(), chunk.size()) !=
                ESP_OK) {
                break;
            }
        }
        ESP_LOGI(TAG, "Credentials for \"%s\" sent (%u bytes)",
                 mock_ap::ssid().c_str(), static_cast<unsigned>(frame.size()));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(mock_ap::init());
    ESP_ERROR_CHECK(mock_udp::start());

    s_wake_queue = xQueueCreate(1, sizeof(uint16_t));
    if (s_wake_queue == nullptr || xTaskCreate(wake_task, "mock_wake", 4096,
                                               nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the wake-up task");
        abort();
    }

    const std::vector<std::uint8_t> adv = camera_mock::ble::advertising_data(
        camera_mock::fixtures::kDevId, camera_mock::fixtures::kProductId,
        camera_mock::fixtures::kBleName);
    ESP_ERROR_CHECK(mock_ble_start(
        adv.data(), adv.size(), camera_mock::fixtures::kBleName, on_ble_write));
    ESP_LOGI(TAG, "Camera mock ready as %s; Wi-Fi \"%s\" after BLE wake-up",
             camera_mock::fixtures::kBleName, mock_ap::ssid().c_str());

#if CONFIG_CAMERA_MOCK_AP_ALWAYS_ON
    mock_ap::start();
#endif
    ESP_ERROR_CHECK(mock_console::start());
}
