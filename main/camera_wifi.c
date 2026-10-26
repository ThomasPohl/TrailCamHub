#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "camera_wifi.h"

#define CAMERA_PROTOCOL_PORT 40611
#define CAMERA_DISCOVERY_ATTEMPTS 3
#define CAMERA_DISCOVERY_RECEIVE_ATTEMPTS 2
#define CAMERA_DISCOVERY_TIMEOUT_MS 500
#define CAMERA_DISCOVERY_LOG_BYTES 64
#define WIFI_CONNECT_MAX_RETRIES 5
#define WIFI_GOT_IP_BIT BIT0

static const char *TAG = "trailcamhub_wifi";
static const uint8_t s_camera_discovery_packet[] = {
    0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
};
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_connect_requested;
static unsigned s_wifi_connect_retries;
static esp_ip4_addr_t s_camera_broadcast;
static bool s_camera_broadcast_valid;

static size_t format_packet_hex(const uint8_t *packet, size_t packet_len,
                                char *output, size_t output_size)
{
    size_t prefix_len = packet_len < CAMERA_DISCOVERY_LOG_BYTES
                            ? packet_len
                            : CAMERA_DISCOVERY_LOG_BYTES;
    size_t output_len = 0;

    if (output_size == 0) {
        return 0;
    }
    output[0] = '\0';
    for (size_t i = 0; i < prefix_len && output_len < output_size; i++) {
        int written = snprintf(output + output_len, output_size - output_len,
                               "%s%02X", i == 0 ? "" : " ", packet[i]);
        if (written < 0 || (size_t)written >= output_size - output_len) {
            break;
        }
        output_len += (size_t)written;
    }
    return output_len == 0 ? 0 : (output_len + 1) / 3;
}

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
    default:
        return "other";
    }
}

static esp_err_t discover_camera(void)
{
    int64_t discovery_started_us = esp_timer_get_time();
    if (!s_camera_broadcast_valid) {
        ESP_LOGE(TAG, "Cannot discover camera because the Wi-Fi broadcast "
                 "address is unknown");
        return ESP_ERR_INVALID_STATE;
    }

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Could not create camera discovery socket: errno %d",
                 errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CAMERA_PROTOCOL_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    ESP_LOGI(TAG, "Binding camera discovery UDP socket to port %u",
             CAMERA_PROTOCOL_PORT);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "Could not bind camera discovery socket to UDP port %u: "
                 "errno %d", CAMERA_PROTOCOL_PORT, errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    int broadcast_enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled,
                   sizeof(broadcast_enabled)) < 0) {
        ESP_LOGE(TAG, "Could not enable UDP broadcast: errno %d", errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = CAMERA_DISCOVERY_TIMEOUT_MS * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set camera discovery timeout: errno %d",
                 errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(CAMERA_PROTOCOL_PORT),
        .sin_addr = {.s_addr = s_camera_broadcast.addr},
    };
    char request_hex[sizeof(s_camera_discovery_packet) * 3 + 1];
    format_packet_hex(s_camera_discovery_packet,
                      sizeof(s_camera_discovery_packet), request_hex,
                      sizeof(request_hex));
    ESP_LOGI(TAG, "Camera discovery target " IPSTR ":%u, source UDP port %u, "
             "payload %u bytes: %s", IP2STR(&s_camera_broadcast),
             CAMERA_PROTOCOL_PORT, CAMERA_PROTOCOL_PORT,
             (unsigned)sizeof(s_camera_discovery_packet), request_hex);

    for (unsigned attempt = 0; attempt < CAMERA_DISCOVERY_ATTEMPTS; attempt++) {
        int64_t attempt_started_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Camera discovery attempt %u/%u", attempt + 1,
                 CAMERA_DISCOVERY_ATTEMPTS);
        ssize_t sent = sendto(socket_fd, s_camera_discovery_packet,
                              sizeof(s_camera_discovery_packet), 0,
                              (struct sockaddr *)&destination,
                              sizeof(destination));
        if (sent < 0 || (size_t)sent != sizeof(s_camera_discovery_packet)) {
            ESP_LOGE(TAG, "Camera discovery send %u/%u failed: errno %d",
                     attempt + 1, CAMERA_DISCOVERY_ATTEMPTS, errno);
            close(socket_fd);
            return ESP_FAIL;
        }
        int64_t sent_at_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Camera discovery attempt %u sent %u bytes",
                 attempt + 1, (unsigned)sent);

        for (unsigned receive_attempt = 0;
             receive_attempt < CAMERA_DISCOVERY_RECEIVE_ATTEMPTS;
             receive_attempt++) {
            uint8_t response[256];
            struct sockaddr_in source = {0};
            socklen_t source_len = sizeof(source);
            ssize_t received = recvfrom(socket_fd, response, sizeof(response),
                                        0, (struct sockaddr *)&source,
                                        &source_len);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ESP_LOGI(TAG, "Camera discovery attempt %u receive wait "
                             "%u/%u timed out after %d ms",
                             attempt + 1, receive_attempt + 1,
                             CAMERA_DISCOVERY_RECEIVE_ATTEMPTS,
                             CAMERA_DISCOVERY_TIMEOUT_MS);
                    break;
                }
                ESP_LOGE(TAG, "Camera discovery receive failed: errno %d",
                         errno);
                close(socket_fd);
                return ESP_FAIL;
            }

            int64_t received_at_us = esp_timer_get_time();
            esp_ip4_addr_t source_ip = {.addr = source.sin_addr.s_addr};
            char response_hex[CAMERA_DISCOVERY_LOG_BYTES * 3 + 1];
            size_t logged_bytes =
                format_packet_hex(response, (size_t)received, response_hex,
                                  sizeof(response_hex));
            ESP_LOGI(TAG, "UDP reply during attempt %u, receive %u/%u, from "
                     IPSTR ":%u, %u bytes, +%lld ms after send / +%lld ms "
                     "total; %s %u bytes: %s%s",
                     attempt + 1, receive_attempt + 1,
                     CAMERA_DISCOVERY_RECEIVE_ATTEMPTS, IP2STR(&source_ip),
                     ntohs(source.sin_port), (unsigned)received,
                     (long long)((received_at_us - sent_at_us) / 1000),
                     (long long)((received_at_us - discovery_started_us) /
                                 1000),
                     (size_t)received <= CAMERA_DISCOVERY_LOG_BYTES
                         ? "full packet"
                         : "first",
                     (unsigned)logged_bytes, response_hex,
                     (size_t)received > CAMERA_DISCOVERY_LOG_BYTES
                         ? " ..." : "");

            if (received >= 2 && response[0] == 0xf1 &&
                response[1] == 0x42) {
                ESP_LOGI(TAG, "Camera discovery response received from "
                         IPSTR ":%u (%u bytes) after %lld ms",
                         IP2STR(&source_ip), ntohs(source.sin_port),
                         (unsigned)received,
                         (long long)((esp_timer_get_time() -
                                      discovery_started_us) /
                                     1000));
                close(socket_fd);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "UDP packet did not match discovery response "
                     "prefix F1 42");
        }
        ESP_LOGI(TAG, "Camera discovery attempt %u finished after %lld ms",
                 attempt + 1,
                 (long long)((esp_timer_get_time() - attempt_started_us) /
                             1000));
        if (attempt + 1 < CAMERA_DISCOVERY_ATTEMPTS) {
            ESP_LOGI(TAG, "Retrying camera discovery");
        }
    }

    close(socket_fd);
    ESP_LOGW(TAG, "No matching camera discovery response after %u attempts "
             "and %lld ms (target " IPSTR ":%u, source UDP port %u)",
             CAMERA_DISCOVERY_ATTEMPTS,
             (long long)((esp_timer_get_time() - discovery_started_us) / 1000),
             IP2STR(&s_camera_broadcast), CAMERA_PROTOCOL_PORT,
             CAMERA_PROTOCOL_PORT);
    return ESP_ERR_TIMEOUT;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Camera Wi-Fi disconnected: reason %u (%s), RSSI %d dBm, "
                 "BSSID %02x:%02x:%02x:%02x:%02x:%02x, retry enabled %s",
                 event->reason, wifi_disconnect_reason_name(event->reason),
                 event->rssi, event->bssid[0], event->bssid[1],
                 event->bssid[2], event->bssid[3], event->bssid[4],
                 event->bssid[5], s_wifi_connect_requested ? "yes" : "no");
        if (!s_wifi_connect_requested) {
            return;
        }
        if (s_wifi_connect_retries >= WIFI_CONNECT_MAX_RETRIES) {
            ESP_LOGE(TAG, "Camera Wi-Fi connection failed after %u retries "
                     "(reason %d)", WIFI_CONNECT_MAX_RETRIES, event->reason);
            s_wifi_connect_requested = false;
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
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event =
            (const wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "Associated with camera AP: BSSID "
                 "%02x:%02x:%02x:%02x:%02x:%02x, channel %u, auth mode %d",
                 event->bssid[0], event->bssid[1], event->bssid[2],
                 event->bssid[3], event->bssid[4], event->bssid[5],
                 event->channel, event->authmode);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        s_camera_broadcast.addr =
            event->ip_info.ip.addr | ~event->ip_info.netmask.addr;
        s_camera_broadcast_valid = true;
        ESP_LOGI(TAG,
                 "Connected to camera Wi-Fi; IP address: " IPSTR
                 ", netmask: " IPSTR ", gateway: " IPSTR
                 ", broadcast: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw), IP2STR(&s_camera_broadcast));
        s_wifi_connect_retries = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

static void camera_wifi_worker(void *arg)
{
    (void)arg;
    while (true) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT, pdTRUE,
                            pdFALSE, portMAX_DELAY);
        discover_camera();
    }
}

esp_err_t camera_wifi_init(void)
{
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

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Could not select station mode");

    BaseType_t task_created = xTaskCreate(camera_wifi_worker, "camera_wifi",
                                          6144, NULL, 5, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Could not create camera Wi-Fi worker task");
        return ESP_ERR_NO_MEM;
    }

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
    wifi_config_t wifi_config = {0};
    if (ssid_len == 0 || ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "Camera Wi-Fi credentials exceed supported field lengths");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    s_wifi_connect_retries = 0;
    s_wifi_connect_requested = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "Could not set camera Wi-Fi credentials");
    s_wifi_connect_requested = true;
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        s_wifi_connect_requested = false;
        ESP_LOGE(TAG, "Could not start camera Wi-Fi connection: %s",
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "Connecting to camera Wi-Fi network \"%s\"", ssid);
    return ESP_OK;
}
