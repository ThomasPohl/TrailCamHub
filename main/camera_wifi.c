#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "lwip/sockets.h"

#include "camera_wifi.h"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_DISCOVERY_DRAIN_MAX_MS 1000
#define CAMERA_DISCOVERY_DRAIN_SILENCE_MS 150
#define CAMERA_DISCOVERY_LOG_BYTES 64
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000
#define CAMERA_PRELOGIN_MAX_PACKET_LEN 128
#define WIFI_CONNECT_MAX_RETRIES 5
#define WIFI_GOT_IP_BIT BIT0

static const char *TAG = "trailcamhub_wifi";
static const uint16_t s_camera_discovery_ports[] = {40611, 3333};
static const uint8_t s_camera_discovery_packet[] = {
    0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
};
static const uint8_t s_prelogin_static_header[] = {
    0x0c, 0xcb, 0x9a, 0x2b, 0x5f, 0x95, 0x1e, 0xb6,
    0x69, 0xdf, 0xaa, 0x37, 0x5a, 0x6b, 0xbe, 0x3e,
    0x76, 0x20, 0x2e, 0x13, 0xc9, 0xd1, 0xaa, 0x36,
    0x31, 0xbe, 0x74, 0xe5,
};
static const uint8_t s_prelogin_aes_key[16] = {
    'a', '0', '1', 'b', 'c', '2', '3', 'e',
    'd', '4', '5', 'f', 'F', '5', '6', 'A',
};
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_connect_requested;
static unsigned s_wifi_connect_retries;
static esp_ip4_addr_t s_camera_gateway;
static bool s_camera_gateway_valid;

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

static void drain_discovery_traffic(int socket_fd,
                                    int64_t discovery_started_us)
{
    struct timeval drain_timeout = {
        .tv_sec = 0,
        .tv_usec = CAMERA_DISCOVERY_DRAIN_SILENCE_MS * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &drain_timeout,
                   sizeof(drain_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set discovery-drain timeout: errno %d", errno);
        return;
    }

    ESP_LOGI(TAG, "Draining queued discovery traffic for up to %u ms "
             "(stop after %u ms of silence)",
             CAMERA_DISCOVERY_DRAIN_MAX_MS,
             CAMERA_DISCOVERY_DRAIN_SILENCE_MS);
    int64_t drain_started_us = esp_timer_get_time();
    unsigned drained_packets = 0;
    unsigned drained_lbcs_packets = 0;

    while ((esp_timer_get_time() - drain_started_us) / 1000 <
           CAMERA_DISCOVERY_DRAIN_MAX_MS) {
        uint8_t packet[256];
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        ssize_t received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                    (struct sockaddr *)&source, &source_len);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGI(TAG, "Discovery drain stopped after %u ms of silence",
                         CAMERA_DISCOVERY_DRAIN_SILENCE_MS);
                break;
            }
            ESP_LOGE(TAG, "Discovery drain receive failed: errno %d", errno);
            break;
        }

        drained_packets++;
        bool is_lbcs = received >= 8 && packet[0] == 0xf1 &&
                       packet[4] == 0x4c && packet[5] == 0x42 &&
                       packet[6] == 0x43 && packet[7] == 0x53;
        if (is_lbcs) {
            drained_lbcs_packets++;
        }
        esp_ip4_addr_t source_ip = {.addr = source.sin_addr.s_addr};
        ESP_LOGD(TAG, "Draining %s packet from " IPSTR ":%u (%u bytes)",
                 is_lbcs ? "LBCS" : "other UDP",
                 IP2STR(&source_ip), ntohs(source.sin_port),
                 (unsigned)received);
    }

    struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = CAMERA_DISCOVERY_TIMEOUT_MS * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not restore discovery receive timeout: errno %d",
                 errno);
    }
    ESP_LOGI(TAG, "Discovery drain finished after %lld ms: %u packets "
             "(%u LBCS)",
             (long long)((esp_timer_get_time() - drain_started_us) / 1000),
             drained_packets, drained_lbcs_packets);
    ESP_LOGI(TAG, "Discovery-to-drain elapsed time: %lld ms",
             (long long)((esp_timer_get_time() - discovery_started_us) /
                         1000));
}

static esp_err_t build_prelogin_packet(uint8_t *packet, size_t packet_capacity,
                                       size_t *packet_len)
{
    static const char *TAG_CRYPTO = "trailcamhub_wifi";
    char json[96];
    uint8_t plaintext[96];
    uint8_t nonce[8];
    uint8_t ciphertext[96];
    size_t ciphertext_len;
    time_t utc_time = time(NULL);

    esp_fill_random(nonce, sizeof(nonce));
    int json_len = snprintf(
        json, sizeof(json),
        "{\"utcTime\":%lld,\"nonce\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}",
        (long long)utc_time, nonce[0], nonce[1], nonce[2], nonce[3], nonce[4],
        nonce[5], nonce[6], nonce[7]);
    if (json_len < 0 || (size_t)json_len >= sizeof(json)) {
        ESP_LOGE(TAG_CRYPTO, "Could not format PRE_LOGIN JSON payload");
        return ESP_ERR_INVALID_SIZE;
    }
    if (utc_time < 1700000000) {
        ESP_LOGW(TAG_CRYPTO, "System UTC time appears unset (%lld); "
                 "PRE_LOGIN timestamp may be rejected",
                 (long long)utc_time);
    }

    size_t padding_len = 16 - ((size_t)json_len % 16);
    ciphertext_len = (size_t)json_len + padding_len;
    if (ciphertext_len > sizeof(ciphertext) ||
        4 + sizeof(s_prelogin_static_header) + ciphertext_len >
            packet_capacity ||
        sizeof(s_prelogin_static_header) + ciphertext_len > UINT16_MAX) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN payload exceeds packet capacity");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(plaintext, json, (size_t)json_len);
    memset(plaintext + json_len, (int)padding_len, padding_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int mbedtls_error =
        mbedtls_aes_setkey_enc(&aes, s_prelogin_aes_key, 128);
    if (mbedtls_error == 0) {
        for (size_t offset = 0; offset < ciphertext_len; offset += 16) {
            mbedtls_error = mbedtls_aes_crypt_ecb(
                &aes, MBEDTLS_AES_ENCRYPT, plaintext + offset,
                ciphertext + offset);
            if (mbedtls_error != 0) {
                break;
            }
        }
    }
    mbedtls_aes_free(&aes);
    memset(plaintext, 0, sizeof(plaintext));
    if (mbedtls_error != 0) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN AES encryption failed: %d",
                 mbedtls_error);
        memset(ciphertext, 0, sizeof(ciphertext));
        return ESP_FAIL;
    }

    size_t body_len = sizeof(s_prelogin_static_header) + ciphertext_len;
    packet[0] = 0xf1;
    packet[1] = 0xf9;
    packet[2] = (uint8_t)(body_len >> 8);
    packet[3] = (uint8_t)body_len;
    memcpy(packet + 4, s_prelogin_static_header,
           sizeof(s_prelogin_static_header));
    memcpy(packet + 4 + sizeof(s_prelogin_static_header), ciphertext,
           ciphertext_len);
    *packet_len = 4 + body_len;
    memset(ciphertext, 0, sizeof(ciphertext));
    ESP_LOGD(TAG_CRYPTO, "Built PRE_LOGIN with %u-byte JSON, %u-byte "
             "ciphertext, UTC timestamp %lld",
             (unsigned)json_len, (unsigned)ciphertext_len,
             (long long)utc_time);
    return ESP_OK;
}

static esp_err_t send_prelogin(int socket_fd,
                               const struct sockaddr_in *camera_address,
                               int64_t discovery_started_us)
{
    uint8_t packet[CAMERA_PRELOGIN_MAX_PACKET_LEN];
    size_t packet_len;
    esp_err_t error =
        build_prelogin_packet(packet, sizeof(packet), &packet_len);
    if (error != ESP_OK) {
        return error;
    }

    esp_ip4_addr_t camera_ip = {.addr = camera_address->sin_addr.s_addr};
    ESP_LOGI(TAG, "Sending AES-encrypted PRE_LOGIN to " IPSTR ":%u "
             "(%u bytes)", IP2STR(&camera_ip),
             ntohs(camera_address->sin_port), (unsigned)packet_len);
    ssize_t sent = sendto(socket_fd, packet, packet_len, 0,
                          (const struct sockaddr *)camera_address,
                          sizeof(*camera_address));
    memset(packet, 0, sizeof(packet));
    if (sent < 0 || (size_t)sent != packet_len) {
        ESP_LOGE(TAG, "PRE_LOGIN send failed: errno %d", errno);
        return ESP_FAIL;
    }
    int64_t sent_at_us = esp_timer_get_time();

    struct timeval response_timeout = {
        .tv_sec = CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec =
            (CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &response_timeout,
                   sizeof(response_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set PRE_LOGIN response timeout: errno %d",
                 errno);
        return ESP_FAIL;
    }

    uint8_t response[256];
    struct sockaddr_in source = {0};
    socklen_t source_len = sizeof(source);
    ssize_t received = recvfrom(socket_fd, response, sizeof(response), 0,
                                (struct sockaddr *)&source, &source_len);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ESP_LOGW(TAG, "No UDP response observed within %u ms after "
                     "PRE_LOGIN; handshake is not verified",
                     CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "PRE_LOGIN response receive failed: errno %d", errno);
        return ESP_FAIL;
    }

    int64_t received_at_us = esp_timer_get_time();
    esp_ip4_addr_t source_ip = {.addr = source.sin_addr.s_addr};
    char response_hex[CAMERA_DISCOVERY_LOG_BYTES * 3 + 1];
    size_t logged_bytes =
        format_packet_hex(response, (size_t)received, response_hex,
                          sizeof(response_hex));
    ESP_LOGI(TAG, "UDP packet after PRE_LOGIN from " IPSTR ":%u, %u bytes, "
             "+%lld ms after send / +%lld ms since discovery; %s %u bytes: "
             "%s%s",
             IP2STR(&source_ip), ntohs(source.sin_port), (unsigned)received,
             (long long)((received_at_us - sent_at_us) / 1000),
             (long long)((received_at_us - discovery_started_us) / 1000),
             (size_t)received <= CAMERA_DISCOVERY_LOG_BYTES
                 ? "full packet"
                 : "first",
             (unsigned)logged_bytes, response_hex,
             (size_t)received > CAMERA_DISCOVERY_LOG_BYTES ? " ..." : "");
    ESP_LOGW(TAG, "PRE_LOGIN response observed; response validation and "
             "ARTEMIS login are not implemented");
    return ESP_OK;
}

static esp_err_t discover_camera(void)
{
    int64_t discovery_started_us = esp_timer_get_time();
    if (!s_camera_gateway_valid) {
        ESP_LOGE(TAG, "Cannot discover camera because the Wi-Fi gateway "
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
        .sin_port = htons(CAMERA_DISCOVERY_LOCAL_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    ESP_LOGI(TAG, "Binding camera UDP socket to local port %u",
             CAMERA_DISCOVERY_LOCAL_PORT);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "Could not bind camera UDP socket to local port %u: "
                 "errno %d", CAMERA_DISCOVERY_LOCAL_PORT, errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    struct timeval receive_timeout = {
        .tv_sec = CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec =
            (CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set camera discovery timeout: errno %d",
                 errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    char request_hex[sizeof(s_camera_discovery_packet) * 3 + 1];
    format_packet_hex(s_camera_discovery_packet,
                      sizeof(s_camera_discovery_packet), request_hex,
                      sizeof(request_hex));
    ESP_LOGI(TAG, "Camera discovery target " IPSTR ", source UDP port %u, "
             "payload %u bytes: %s", IP2STR(&s_camera_gateway),
             CAMERA_DISCOVERY_LOCAL_PORT,
             (unsigned)sizeof(s_camera_discovery_packet), request_hex);

    for (size_t port_index = 0;
         port_index < sizeof(s_camera_discovery_ports) /
                          sizeof(s_camera_discovery_ports[0]);
         port_index++) {
        struct sockaddr_in destination = {
            .sin_family = AF_INET,
            .sin_port = htons(s_camera_discovery_ports[port_index]),
            .sin_addr = {.s_addr = s_camera_gateway.addr},
        };
        ssize_t sent = sendto(socket_fd, s_camera_discovery_packet,
                              sizeof(s_camera_discovery_packet), 0,
                              (struct sockaddr *)&destination,
                              sizeof(destination));
        if (sent < 0 || (size_t)sent != sizeof(s_camera_discovery_packet)) {
            ESP_LOGE(TAG, "Camera discovery send to " IPSTR ":%u failed: "
                     "errno %d", IP2STR(&s_camera_gateway),
                     s_camera_discovery_ports[port_index], errno);
            close(socket_fd);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Camera discovery sent %u bytes to " IPSTR ":%u",
                 (unsigned)sent, IP2STR(&s_camera_gateway),
                 s_camera_discovery_ports[port_index]);
    }

    while ((esp_timer_get_time() - discovery_started_us) / 1000 <
           CAMERA_DISCOVERY_TIMEOUT_MS) {
        uint8_t response[256];
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        ssize_t received = recvfrom(socket_fd, response, sizeof(response), 0,
                                    (struct sockaddr *)&source, &source_len);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "Camera discovery receive failed: errno %d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }

        int64_t received_at_us = esp_timer_get_time();
        esp_ip4_addr_t source_ip = {.addr = source.sin_addr.s_addr};
        char response_hex[CAMERA_DISCOVERY_LOG_BYTES * 3 + 1];
        size_t logged_bytes =
            format_packet_hex(response, (size_t)received, response_hex,
                              sizeof(response_hex));
        ESP_LOGI(TAG, "UDP reply from " IPSTR ":%u, %u bytes, +%lld ms; "
                 "%s %u bytes: %s%s",
                 IP2STR(&source_ip), ntohs(source.sin_port),
                 (unsigned)received,
                 (long long)((received_at_us - discovery_started_us) / 1000),
                 (size_t)received <= CAMERA_DISCOVERY_LOG_BYTES
                     ? "full packet"
                     : "first",
                 (unsigned)logged_bytes, response_hex,
                 (size_t)received > CAMERA_DISCOVERY_LOG_BYTES ? " ..." : "");

        if (received > 4 && response[0] == 0xf1 &&
            source.sin_addr.s_addr == s_camera_gateway.addr) {
            ESP_LOGI(TAG, "Camera UDP response accepted; using source port "
                     "%u for PRE_LOGIN", ntohs(source.sin_port));
            drain_discovery_traffic(socket_fd, discovery_started_us);
            esp_err_t prelogin_error =
                send_prelogin(socket_fd, &source, discovery_started_us);
            close(socket_fd);
            return prelogin_error;
        }
        ESP_LOGW(TAG, "UDP packet did not match camera response "
                 "(expected F1 prefix from gateway " IPSTR ")",
                 IP2STR(&s_camera_gateway));
    }

    close(socket_fd);
    ESP_LOGW(TAG, "No matching camera discovery response after %lld ms "
             "(target " IPSTR ", source UDP port %u)",
             (long long)((esp_timer_get_time() - discovery_started_us) / 1000),
             IP2STR(&s_camera_gateway), CAMERA_DISCOVERY_LOCAL_PORT);
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
        s_camera_gateway = event->ip_info.gw;
        s_camera_gateway_valid = s_camera_gateway.addr != 0;
        ESP_LOGI(TAG,
                 "Connected to camera Wi-Fi; IP address: " IPSTR
                 ", netmask: " IPSTR ", gateway: " IPSTR
                 ", discovery target: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw), IP2STR(&s_camera_gateway));
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
        esp_err_t error = discover_camera();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Camera discovery/PRE_LOGIN stage ended with %s",
                     esp_err_to_name(error));
        }
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
