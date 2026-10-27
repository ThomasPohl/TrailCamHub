/*
 * This module manages the Wi-Fi station that joins the camera access point
 * and reports connection changes through callbacks.
 */

#include <atomic>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "camera_wifi.h"

#define WIFI_CONNECT_MAX_RETRIES 5

static const char *TAG = "trailcamhub_wifi";
static camera_wifi_callbacks_t s_callbacks;
// Set while a connection is wanted; cleared by camera_wifi_disconnect().
static std::atomic<bool> s_wifi_connect_requested;
// Set between camera_wifi_connect() and obtaining an IP address.
static std::atomic<bool> s_joining;
static unsigned s_wifi_connect_retries;

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "access point not found";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_ASSOC_LEAVE:
        return "left the network";
    default:
        return "other";
    }
}

static void handle_disconnected(const wifi_event_sta_disconnected_t *event)
{
    bool requested = s_wifi_connect_requested.load();
    ESP_LOGW(TAG,
             "Camera Wi-Fi disconnected: reason %u (%s), RSSI %d dBm, "
             "BSSID %02x:%02x:%02x:%02x:%02x:%02x, retry enabled %s",
             event->reason, wifi_disconnect_reason_name(event->reason),
             event->rssi, event->bssid[0], event->bssid[1], event->bssid[2],
             event->bssid[3], event->bssid[4], event->bssid[5],
             requested && s_joining.load() ? "yes" : "no");
    if (!requested) {
        return;
    }
    if (!s_joining.load()) {
        // An established connection was lost; the caller decides what next.
        s_wifi_connect_requested = false;
        if (s_callbacks.on_disconnected != NULL) {
            s_callbacks.on_disconnected(event->reason);
        }
        return;
    }
    if (s_wifi_connect_retries >= WIFI_CONNECT_MAX_RETRIES) {
        ESP_LOGE(TAG,
                 "Camera Wi-Fi connection failed after %u retries "
                 "(reason %d)",
                 WIFI_CONNECT_MAX_RETRIES, event->reason);
        s_wifi_connect_requested = false;
        s_joining = false;
        if (s_callbacks.on_failed != NULL) {
            s_callbacks.on_failed(event->reason);
        }
        return;
    }

    s_wifi_connect_retries++;
    ESP_LOGW(TAG, "Retrying camera Wi-Fi connection: attempt %u/%u",
             s_wifi_connect_retries, WIFI_CONNECT_MAX_RETRIES);
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Camera Wi-Fi reconnect failed: %s",
                 esp_err_to_name(error));
        s_wifi_connect_requested = false;
        s_joining = false;
        if (s_callbacks.on_failed != NULL) {
            s_callbacks.on_failed(event->reason);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        handle_disconnected(
            static_cast<const wifi_event_sta_disconnected_t *>(event_data));
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event =
            (const wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG,
                 "Associated with camera AP: BSSID "
                 "%02x:%02x:%02x:%02x:%02x:%02x, channel %u, auth mode %d",
                 event->bssid[0], event->bssid[1], event->bssid[2],
                 event->bssid[3], event->bssid[4], event->bssid[5],
                 event->channel, event->authmode);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG,
                 "Connected to camera Wi-Fi; IP address: " IPSTR
                 ", netmask: " IPSTR ", gateway: " IPSTR
                 ", discovery target: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.gw));
        s_wifi_connect_retries = 0;
        s_joining = false;
        if (s_wifi_connect_requested.load() &&
            s_callbacks.on_connected != NULL) {
            s_callbacks.on_connected(&event->ip_info.gw);
        }
    }
}

esp_err_t camera_wifi_init(const camera_wifi_callbacks_t *callbacks)
{
    if (callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callbacks = *callbacks;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG,
                        "Could not initialize network interface");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "Could not create default event loop");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi station interface");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG,
                        "Could not initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Could not restrict Wi-Fi credentials to RAM");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Could not select station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Could not start Wi-Fi");
    ESP_LOGI(TAG, "Camera Wi-Fi station is ready");
    return ESP_OK;
}

esp_err_t camera_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    wifi_config_t wifi_config = {};
    if (ssid_len == 0 || ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG,
                 "Camera Wi-Fi credentials exceed supported field lengths");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    s_wifi_connect_retries = 0;
    s_wifi_connect_requested = false;
    s_joining = false;
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    ESP_RETURN_ON_ERROR(error, TAG, "Could not set camera Wi-Fi credentials");
    s_joining = true;
    s_wifi_connect_requested = true;
    error = esp_wifi_connect();
    if (error != ESP_OK) {
        s_wifi_connect_requested = false;
        s_joining = false;
        ESP_LOGE(TAG, "Could not start camera Wi-Fi connection: %s",
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "Connecting to camera Wi-Fi network \"%s\"", ssid);
    return ESP_OK;
}

esp_err_t camera_wifi_disconnect(void)
{
    s_wifi_connect_requested = false;
    s_joining = false;
    esp_err_t error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Could not leave camera Wi-Fi: %s",
                 esp_err_to_name(error));
        return error;
    }
    return ESP_OK;
}
