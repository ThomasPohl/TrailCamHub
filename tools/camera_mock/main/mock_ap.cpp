#include "mock_ap.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ble_frames.hpp"

namespace mock_ap
{

namespace
{

const char *TAG = "mock_ap";
constexpr EventBits_t kStartedBit = BIT0;
constexpr TickType_t kStartTimeout = pdMS_TO_TICKS(5000);

esp_netif_t *s_netif;
EventGroupHandle_t s_events;
std::atomic<bool> s_running{false};
std::atomic<int> s_stations{0};
std::uint8_t s_mac[6];

void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_AP_START:
        s_running = true;
        xEventGroupSetBits(s_events, kStartedBit);
        ESP_LOGI(TAG, "Access point \"%s\" up at 192.168.43.1", ssid().c_str());
        break;
    case WIFI_EVENT_AP_STOP:
        s_running = false;
        s_stations = 0;
        xEventGroupClearBits(s_events, kStartedBit);
        ESP_LOGI(TAG, "Access point stopped");
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
        s_stations++;
        ESP_LOGI(TAG, "Station " MACSTR " joined (AID %d)", MAC2STR(event->mac),
                 event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const auto *event =
            static_cast<wifi_event_ap_stadisconnected_t *>(data);
        if (s_stations > 0) {
            s_stations--;
        }
        ESP_LOGI(TAG, "Station " MACSTR " left (reason %d)",
                 MAC2STR(event->mac), event->reason);
        break;
    }
    default:
        break;
    }
}

}  // namespace

esp_err_t init()
{
    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_WIFI_SOFTAP));

    s_netif = esp_netif_create_default_wifi_ap();
    // The camera's address; the hub takes the gateway from DHCP.
    esp_netif_ip_info_t ip = {};
    esp_netif_set_ip4_addr(&ip.ip, 192, 168, 43, 1);
    esp_netif_set_ip4_addr(&ip.gw, 192, 168, 43, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    // The DHCP server must be stopped to change the address; it starts again
    // with the interface.
    esp_netif_dhcps_stop(s_netif);
    esp_err_t err = esp_netif_set_ip_info(s_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set the AP address: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_dhcps_start(s_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP server: %s", esp_err_to_name(err));
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, nullptr));
    return ESP_OK;
}

bool start()
{
    if (s_running) {
        return true;
    }
    wifi_config_t config = {};
    const std::string name = ssid();
    const std::string pwd = password();
    std::memcpy(config.ap.ssid, name.data(), name.size());
    config.ap.ssid_len = static_cast<std::uint8_t>(name.size());
    std::strncpy(reinterpret_cast<char *>(config.ap.password), pwd.c_str(),
                 sizeof(config.ap.password) - 1);
    config.ap.channel = CONFIG_CAMERA_MOCK_AP_CHANNEL;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2;
    config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the access point: %s",
                 esp_err_to_name(err));
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(s_events, kStartedBit, pdFALSE,
                                                 pdTRUE, kStartTimeout);
    return (bits & kStartedBit) != 0;
}

void stop()
{
    if (s_running) {
        esp_wifi_stop();
    }
}

bool running()
{
    return s_running;
}

std::string ssid()
{
    return camera_mock::ble::ap_ssid(s_mac);
}

std::string bssid()
{
    return camera_mock::ble::format_mac(s_mac);
}

std::string password()
{
    return CONFIG_CAMERA_MOCK_AP_PASSWORD;
}

int station_count()
{
    return s_stations;
}

}  // namespace mock_ap
