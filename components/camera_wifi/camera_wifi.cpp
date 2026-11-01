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
#include "disconnect_policy.hpp"

#define WIFI_CONNECT_MAX_RETRIES 5

static const char *TAG = "trailcamhub_wifi";
static camera_wifi_callbacks_t s_callbacks;
// Set while a connection is wanted; cleared by camera_wifi_disconnect().
static std::atomic<bool> s_wifi_connect_requested;
// Set between camera_wifi_connect() and obtaining an IP address.
static std::atomic<bool> s_joining;
static unsigned s_wifi_connect_retries;
// The network being joined or connected, to tell its events from late events
// of the network left before. Written before the flags above are set.
static char s_target_ssid[33];
static size_t s_target_ssid_len;
static const char *s_target_label = "camera";

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
    using wifipolicy::DisconnectAction;
    const bool requested = s_wifi_connect_requested.load();
    const bool joining = s_joining.load();
    // ssid_len is set by the driver; clamp it in case it is not.
    const size_t event_ssid_len = event->ssid_len <= sizeof(event->ssid)
                                      ? event->ssid_len
                                      : sizeof(event->ssid);
    const bool for_target = wifipolicy::is_for_target(
        event->ssid, event_ssid_len, s_target_ssid, s_target_ssid_len);
    const DisconnectAction action = wifipolicy::on_disconnected(
        requested, joining, for_target, s_wifi_connect_retries,
        WIFI_CONNECT_MAX_RETRIES);
    ESP_LOGW(TAG,
             "Wi-Fi disconnected from \"%.*s\": reason %u (%s), RSSI %d dBm, "
             "BSSID %02x:%02x:%02x:%02x:%02x:%02x, %s",
             (int)event_ssid_len, (const char *)event->ssid, event->reason,
             wifi_disconnect_reason_name(event->reason), event->rssi,
             event->bssid[0], event->bssid[1], event->bssid[2], event->bssid[3],
             event->bssid[4], event->bssid[5],
             action == DisconnectAction::retry    ? "retrying"
             : action == DisconnectAction::ignore ? "ignored"
                                                  : "not retrying");
    // Back to modem sleep, which BLE coexistence requires, before the BLE
    // stack may start again. Not for an ignored event: the station may be
    // joining or connected to another network by now.
    if (action != DisconnectAction::ignore) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    switch (action) {
    case DisconnectAction::ignore:
        return;
    case DisconnectAction::report_lost:
        // An established connection was lost; the caller decides what next.
        s_wifi_connect_requested = false;
        if (s_callbacks.on_disconnected != NULL) {
            s_callbacks.on_disconnected(event->reason);
        }
        return;
    case DisconnectAction::give_up:
        ESP_LOGE(TAG, "%s Wi-Fi connection failed after %u retries (reason %d)",
                 s_target_label, WIFI_CONNECT_MAX_RETRIES, event->reason);
        s_wifi_connect_requested = false;
        s_joining = false;
        if (s_callbacks.on_failed != NULL) {
            s_callbacks.on_failed(event->reason);
        }
        return;
    case DisconnectAction::retry:
        break;
    }

    s_wifi_connect_retries++;
    ESP_LOGW(TAG, "Retrying %s Wi-Fi connection: attempt %u/%u", s_target_label,
             s_wifi_connect_retries, WIFI_CONNECT_MAX_RETRIES);
    esp_err_t error = esp_wifi_connect();
    if (error == ESP_ERR_WIFI_CONN) {
        // The driver is already connecting; its outcome arrives as an event.
        ESP_LOGW(TAG, "%s Wi-Fi is already connecting; waiting",
                 s_target_label);
        return;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s Wi-Fi reconnect failed: %s", s_target_label,
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
                 "Associated with %s AP: BSSID "
                 "%02x:%02x:%02x:%02x:%02x:%02x, channel %u, auth mode %d",
                 s_target_label, event->bssid[0], event->bssid[1],
                 event->bssid[2], event->bssid[3], event->bssid[4],
                 event->bssid[5], event->channel, event->authmode);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG,
                 "Connected to %s Wi-Fi; IP address: " IPSTR ", netmask: " IPSTR
                 ", gateway: " IPSTR ", discovery target: " IPSTR,
                 s_target_label, IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw),
                 IP2STR(&event->ip_info.gw));
        s_wifi_connect_retries = 0;
        s_joining = false;
        // No power save while the camera streams: in modem sleep, outgoing
        // ACKs queue up until the radio wakes, and at a few hundred frames
        // per second the TX buffers run out (sends fail with ENOMEM). BLE is
        // released during the session, so coexistence does not need it.
        const esp_err_t ps_error = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_error != ESP_OK) {
            ESP_LOGW(TAG, "Could not switch Wi-Fi power save off: %s",
                     esp_err_to_name(ps_error));
        }
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

// Joins `ssid`. The camera access point gets the settings verified on the
// hardware (any auth mode, no PMF); the home network allows PMF so WPA3
// transition networks work.
static esp_err_t connect_network(const char *ssid, const char *password,
                                 bool home_network)
{
    const char *label = home_network ? "home" : "camera";
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    wifi_config_t wifi_config = {};
    if (ssid_len == 0 || ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "%s Wi-Fi credentials exceed supported field lengths",
                 label);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = home_network;
    wifi_config.sta.pmf_cfg.required = false;

    s_wifi_connect_retries = 0;
    s_wifi_connect_requested = false;
    s_joining = false;
    memcpy(s_target_ssid, ssid, ssid_len);
    s_target_ssid[ssid_len] = '\0';
    s_target_ssid_len = ssid_len;
    s_target_label = label;
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    ESP_RETURN_ON_ERROR(error, TAG, "Could not set %s Wi-Fi credentials",
                        label);
    s_joining = true;
    s_wifi_connect_requested = true;
    error = esp_wifi_connect();
    if (error != ESP_OK) {
        s_wifi_connect_requested = false;
        s_joining = false;
        ESP_LOGE(TAG, "Could not start %s Wi-Fi connection: %s", label,
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "Connecting to %s Wi-Fi network \"%s\"", label, ssid);
    return ESP_OK;
}

esp_err_t camera_wifi_connect(const char *ssid, const char *password)
{
    return connect_network(ssid, password, false);
}

esp_err_t camera_wifi_connect_home(const char *ssid, const char *password)
{
    return connect_network(ssid, password, true);
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
