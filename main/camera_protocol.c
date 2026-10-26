#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "lwip/sockets.h"

#include "camera_protocol.h"
#include "camera_udp_transport.h"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_DISCOVERY_DRAIN_MAX_MS 1000
#define CAMERA_DISCOVERY_DRAIN_SILENCE_MS 150
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000
#define CAMERA_PRELOGIN_MAX_PACKET_LEN 128
#define CAMERA_RUDP_DATA_TYPE 0xd0
#define CAMERA_RUDP_ACK_TYPE 0xd1
#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500
#define CAMERA_RUDP_RETRANSMIT_INTERVAL_MS 800

static const char *TAG = "trailcamhub_protocol";
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
static const uint8_t s_artemis_hello_body[] = {
    0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xad, 0x00, 0x00, 0x00, 0x4a, 0x38, 0x57, 0x57,
    0x75, 0x51, 0x44, 0x50, 0x6d, 0x59, 0x53, 0x4c,
    0x66, 0x75, 0x2f, 0x67, 0x58, 0x41, 0x47, 0x2b,
    0x55, 0x71, 0x62, 0x42, 0x79, 0x35, 0x35, 0x4b,
    0x50, 0x32, 0x69, 0x45, 0x32, 0x35, 0x51, 0x50,
    0x4e, 0x6f, 0x66, 0x7a, 0x6e, 0x30, 0x34, 0x30,
    0x2b, 0x4e, 0x49, 0x39, 0x67, 0x37, 0x7a, 0x65,
    0x58, 0x4c, 0x6b, 0x49, 0x70, 0x58, 0x70, 0x43,
    0x30, 0x37, 0x53, 0x58, 0x76, 0x6f, 0x73, 0x72,
    0x57, 0x73, 0x63, 0x31, 0x6d, 0x38, 0x6d, 0x78,
    0x6e, 0x71, 0x36, 0x68, 0x4d, 0x69, 0x4b, 0x77,
    0x65, 0x50, 0x62, 0x4b, 0x4a, 0x55, 0x77, 0x76,
    0x53, 0x76, 0x71, 0x5a, 0x62, 0x36, 0x73, 0x30,
    0x73, 0x6c, 0x31, 0x73, 0x66, 0x7a, 0x68, 0x33,
    0x53, 0x35, 0x30, 0x70, 0x70, 0x30, 0x74, 0x75,
    0x32, 0x4b, 0x65, 0x77, 0x69, 0x30, 0x50, 0x69,
    0x44, 0x63, 0x76, 0x58, 0x71, 0x58, 0x4d, 0x32,
    0x68, 0x50, 0x6c, 0x4e, 0x6c, 0x68, 0x47, 0x53,
    0x69, 0x33, 0x46, 0x55, 0x41, 0x76, 0x2b, 0x50,
    0x64, 0x79, 0x35, 0x68, 0x2f, 0x72, 0x78, 0x38,
    0x2b, 0x47, 0x74, 0x37, 0x37, 0x54, 0x68, 0x45,
    0x2b, 0x72, 0x64, 0x31, 0x44, 0x6d, 0x45, 0x3d,
    0x00,
};
static const uint8_t s_artemis_magic_body_1[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t s_artemis_magic_body_2[] = {0x00, 0x00};
static const uint8_t s_artemis_heartbeat_prefix[] = {
    0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00,
    0x02, 0x00, 0x00, 0x00,
};
static const uint8_t s_artemis_heartbeat_suffix[] = {
    0x00, 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x4d,
    0x7a, 0x6c, 0x42, 0x33, 0x36, 0x58, 0x2f, 0x49,
    0x56, 0x6f, 0x38, 0x5a, 0x7a, 0x49, 0x35, 0x72,
    0x47, 0x39, 0x6a, 0x31, 0x77, 0x3d, 0x3d, 0x00,
};
static esp_ip4_addr_t s_camera_gateway;
static bool s_camera_gateway_valid;
static uint8_t s_camera_rudp_sequence;

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
        ssize_t received = camera_udp_receive_packet(
            socket_fd, packet, sizeof(packet), &source, "discovery drain");
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

static bool is_camera_data_packet(const uint8_t *packet, size_t packet_len)
{
    if (packet_len < 8 || packet[0] != 0xf1 ||
        packet[1] != CAMERA_RUDP_DATA_TYPE ||
        packet[4] != CAMERA_RUDP_ACK_TYPE ||
        packet[5] != 0 || packet[6] != 0) {
        return false;
    }

    uint16_t body_len = ((uint16_t)packet[2] << 8) | packet[3];
    return body_len >= 4 && (size_t)body_len == packet_len - 4;
}

static esp_err_t build_rudp_packet(uint8_t packet_type, const uint8_t *payload,
                                   size_t payload_len, uint8_t *packet,
                                   size_t packet_capacity,
                                   size_t *packet_len,
                                   uint8_t *sent_sequence)
{
    size_t body_len = payload_len + 4;
    size_t total_len = body_len + 4;
    if (total_len > packet_capacity || body_len > UINT16_MAX ||
        (payload_len > 0 && payload == NULL) || packet == NULL ||
        packet_len == NULL) {
        ESP_LOGE(TAG, "RUDP packet exceeds supported size");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t sequence = (uint8_t)(s_camera_rudp_sequence % 254 + 1);
    s_camera_rudp_sequence = sequence;
    packet[0] = 0xf1;
    packet[1] = packet_type;
    packet[2] = (uint8_t)(body_len >> 8);
    packet[3] = (uint8_t)body_len;
    packet[4] = CAMERA_RUDP_ACK_TYPE;
    packet[5] = 0;
    packet[6] = 0;
    packet[7] = sequence;
    if (payload_len > 0) {
        memcpy(packet + 8, payload, payload_len);
    }
    *packet_len = total_len;
    if (sent_sequence != NULL) {
        *sent_sequence = sequence;
    }
    return ESP_OK;
}

static esp_err_t send_rudp_packet(int socket_fd,
                                  const struct sockaddr_in *camera_address,
                                  const char *packet_name,
                                  uint8_t packet_type,
                                  const uint8_t *payload, size_t payload_len,
                                  uint8_t *sent_sequence)
{
    uint8_t packet[256];
    size_t packet_len;
    esp_err_t error = build_rudp_packet(packet_type, payload, payload_len,
                                        packet, sizeof(packet), &packet_len,
                                        sent_sequence);
    if (error != ESP_OK) {
        return error;
    }

    ssize_t sent = camera_udp_send_packet(socket_fd, packet, packet_len,
                                   camera_address, packet_name);
    if (sent < 0 || (size_t)sent != packet_len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_data_ack(int socket_fd,
                               const struct sockaddr_in *camera_address,
                               uint8_t received_sequence)
{
    uint8_t ack_payload[] = {
        0x00, received_sequence, 0x00, received_sequence,
    };
    uint8_t sequence;
    esp_err_t error = send_rudp_packet(
        socket_fd, camera_address, "ARTEMIS DATA ACK",
        CAMERA_RUDP_ACK_TYPE, ack_payload, sizeof(ack_payload), &sequence);
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "ARTEMIS DATA ACK acknowledges received sequence %u "
             "using local sequence %u", received_sequence, sequence);
    return ESP_OK;
}

static esp_err_t send_artemis_handshake(
    int socket_fd, const struct sockaddr_in *camera_address)
{
    esp_err_t error = send_rudp_packet(
        socket_fd, camera_address, "ARTEMIS Hello", CAMERA_RUDP_DATA_TYPE,
        s_artemis_hello_body, sizeof(s_artemis_hello_body), NULL);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    error = send_rudp_packet(
        socket_fd, camera_address, "ARTEMIS Magic 1", CAMERA_RUDP_ACK_TYPE,
        s_artemis_magic_body_1, sizeof(s_artemis_magic_body_1), NULL);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    return send_rudp_packet(
        socket_fd, camera_address, "ARTEMIS Magic 2", CAMERA_RUDP_ACK_TYPE,
        s_artemis_magic_body_2, sizeof(s_artemis_magic_body_2), NULL);
}

static esp_err_t wait_for_artemis_packets(
    int socket_fd, const struct sockaddr_in *camera_address,
    int64_t discovery_started_us)
{
    uint8_t heartbeat_body[sizeof(s_artemis_heartbeat_prefix) + 1 +
                           sizeof(s_artemis_heartbeat_suffix)];
    memcpy(heartbeat_body, s_artemis_heartbeat_prefix,
           sizeof(s_artemis_heartbeat_prefix));
    memcpy(heartbeat_body + sizeof(s_artemis_heartbeat_prefix) + 1,
           s_artemis_heartbeat_suffix, sizeof(s_artemis_heartbeat_suffix));

    uint8_t pending_packet[256];
    size_t pending_packet_len = 0;
    uint8_t pending_sequence = 0;
    uint8_t heartbeat_command_count = 1;
    bool waiting_for_ack = false;
    int64_t last_heartbeat_us = 0;
    int64_t last_retry_us = 0;
    ESP_LOGI(TAG, "Starting ARTEMIS heartbeat stop-and-wait loop");

    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (!waiting_for_ack &&
            (last_heartbeat_us == 0 ||
             now_us - last_heartbeat_us >=
                 CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL)) {
            heartbeat_command_count =
                (uint8_t)((heartbeat_command_count + 1) % 255);
            heartbeat_body[sizeof(s_artemis_heartbeat_prefix)] =
                heartbeat_command_count;
            esp_err_t error = build_rudp_packet(
                CAMERA_RUDP_DATA_TYPE, heartbeat_body,
                sizeof(heartbeat_body), pending_packet,
                sizeof(pending_packet), &pending_packet_len,
                &pending_sequence);
            if (error != ESP_OK) {
                return error;
            }
            ssize_t sent = camera_udp_send_packet(
                socket_fd, pending_packet, pending_packet_len,
                camera_address, "ARTEMIS heartbeat");
            if (sent < 0 || (size_t)sent != pending_packet_len) {
                return ESP_FAIL;
            }
            waiting_for_ack = true;
            last_heartbeat_us = now_us;
            last_retry_us = now_us;
            ESP_LOGI(TAG, "ARTEMIS heartbeat command %u, sequence %u "
                     "awaiting ACK",
                     heartbeat_command_count, pending_sequence);
        } else if (waiting_for_ack &&
                   now_us - last_retry_us >=
                       CAMERA_RUDP_RETRANSMIT_INTERVAL_MS * 1000LL) {
            ssize_t sent = camera_udp_send_packet(
                socket_fd, pending_packet, pending_packet_len,
                camera_address, "ARTEMIS heartbeat retransmission");
            if (sent < 0 || (size_t)sent != pending_packet_len) {
                return ESP_FAIL;
            }
            last_retry_us = now_us;
            ESP_LOGW(TAG, "Retransmitted ARTEMIS heartbeat sequence %u",
                     pending_sequence);
        }

        struct timeval receive_timeout = {
            .tv_sec = 0,
            .tv_usec = CAMERA_RUDP_RECEIVE_TIMEOUT_MS * 1000,
        };
        if (waiting_for_ack) {
            int64_t retry_remaining_us =
                CAMERA_RUDP_RETRANSMIT_INTERVAL_MS * 1000LL -
                (esp_timer_get_time() - last_retry_us);
            if (retry_remaining_us < receive_timeout.tv_usec) {
                receive_timeout.tv_usec =
                    retry_remaining_us > 0 ? retry_remaining_us : 1;
            }
        } else if (last_heartbeat_us != 0) {
            int64_t heartbeat_remaining_us =
                CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL -
                (esp_timer_get_time() - last_heartbeat_us);
            if (heartbeat_remaining_us < receive_timeout.tv_usec) {
                receive_timeout.tv_usec =
                    heartbeat_remaining_us > 0 ? heartbeat_remaining_us : 1;
            }
        }
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                       sizeof(receive_timeout)) < 0) {
            ESP_LOGE(TAG, "Could not set ARTEMIS receive timeout: errno %d",
                     errno);
            return ESP_FAIL;
        }

        uint8_t packet[512];
        struct sockaddr_in source = {0};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, packet, sizeof(packet), &source, "ARTEMIS");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "ARTEMIS packet receive failed: errno %d", errno);
            return ESP_FAIL;
        }

        if (source.sin_addr.s_addr != camera_address->sin_addr.s_addr ||
            source.sin_port != camera_address->sin_port) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }

        ESP_LOGI(TAG, "ARTEMIS packet received +%lld ms since discovery",
                 (long long)((esp_timer_get_time() - discovery_started_us) /
                             1000));

        if (is_camera_data_packet(packet, (size_t)received)) {
            uint8_t received_sequence = packet[7];
            ESP_LOGI(TAG, "Received ARTEMIS DATA packet, sequence %u",
                     received_sequence);
            esp_err_t error =
                send_data_ack(socket_fd, &source, received_sequence);
            if (error != ESP_OK) {
                return error;
            }
        } else if (received >= 12 && packet[0] == 0xf1 &&
                   packet[1] == CAMERA_RUDP_ACK_TYPE) {
            uint16_t body_len = ((uint16_t)packet[2] << 8) | packet[3];
            bool acknowledges_pending =
                waiting_for_ack && body_len == (size_t)received - 4 &&
                body_len >= 8 && packet[8] == 0 &&
                packet[9] == pending_sequence && packet[10] == 0 &&
                packet[11] == pending_sequence;
            if (acknowledges_pending) {
                ESP_LOGI(TAG, "Heartbeat sequence %u acknowledged",
                         pending_sequence);
                waiting_for_ack = false;
            } else {
                ESP_LOGI(TAG, "Received ARTEMIS ACK packet not matching "
                         "pending heartbeat sequence %u",
                         pending_sequence);
            }
        } else {
            ESP_LOGW(TAG, "Ignoring unrecognized ARTEMIS packet");
        }
    }
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

    ssize_t sent = camera_udp_send_packet(socket_fd, packet, packet_len,
                                   camera_address, "AES-encrypted PRE_LOGIN");
    if (sent < 0 || (size_t)sent != packet_len) {
        memset(packet, 0, sizeof(packet));
        return ESP_FAIL;
    }
    memset(packet, 0, sizeof(packet));
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

    int64_t response_deadline_us =
        sent_at_us + CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS * 1000;
    while (true) {
        int64_t remaining_us = response_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            ESP_LOGW(TAG, "No DATA packet received within %u ms after "
                     "PRE_LOGIN; handshake is not verified",
                     CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        struct timeval response_timeout = {
            .tv_sec = remaining_us / 1000000,
            .tv_usec = remaining_us % 1000000,
        };
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &response_timeout,
                       sizeof(response_timeout)) < 0) {
            ESP_LOGE(TAG, "Could not set PRE_LOGIN response timeout: errno %d",
                     errno);
            return ESP_FAIL;
        }

        uint8_t response[256];
        struct sockaddr_in source = {0};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "PRE_LOGIN");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "No DATA packet received within %u ms after "
                         "PRE_LOGIN; handshake is not verified",
                         CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
                return ESP_ERR_TIMEOUT;
            }
            ESP_LOGE(TAG, "PRE_LOGIN response receive failed: errno %d", errno);
            return ESP_FAIL;
        }

        int64_t received_at_us = esp_timer_get_time();
        ESP_LOGI(TAG, "PRE_LOGIN response received +%lld ms after send / "
                 "+%lld ms since discovery",
                 (long long)((received_at_us - sent_at_us) / 1000),
                 (long long)((received_at_us - discovery_started_us) / 1000));

        bool from_camera =
            source.sin_addr.s_addr == camera_address->sin_addr.s_addr &&
            source.sin_port == camera_address->sin_port;
        if (from_camera && is_camera_data_packet(response, (size_t)received)) {
            ESP_LOGI(TAG, "Received ARTEMIS DATA packet, sequence %u",
                     response[7]);
            esp_err_t ack_error =
                send_data_ack(socket_fd, &source, response[7]);
            if (ack_error != ESP_OK) {
                return ack_error;
            }
            error = send_artemis_handshake(socket_fd, &source);
            if (error != ESP_OK) {
                return error;
            }
            return wait_for_artemis_packets(socket_fd, &source,
                                            discovery_started_us);
        }

        ESP_LOGW(TAG, "Ignoring non-DATA UDP packet while waiting for "
                 "PRE_LOGIN response");
    }
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

    ESP_LOGI(TAG, "Camera discovery target " IPSTR ", source UDP port %u, "
             "payload %u bytes", IP2STR(&s_camera_gateway),
             CAMERA_DISCOVERY_LOCAL_PORT,
             (unsigned)sizeof(s_camera_discovery_packet));

    for (size_t port_index = 0;
         port_index < sizeof(s_camera_discovery_ports) /
                          sizeof(s_camera_discovery_ports[0]);
         port_index++) {
        struct sockaddr_in destination = {
            .sin_family = AF_INET,
            .sin_port = htons(s_camera_discovery_ports[port_index]),
            .sin_addr = {.s_addr = s_camera_gateway.addr},
        };
        ssize_t sent = camera_udp_send_packet(
            socket_fd, s_camera_discovery_packet,
            sizeof(s_camera_discovery_packet), &destination,
            "LBCS discovery");
        if (sent < 0 || (size_t)sent != sizeof(s_camera_discovery_packet)) {
            close(socket_fd);
            return ESP_FAIL;
        }
    }

    while ((esp_timer_get_time() - discovery_started_us) / 1000 <
           CAMERA_DISCOVERY_TIMEOUT_MS) {
        uint8_t response[256];
        struct sockaddr_in source = {0};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source,
            "camera discovery");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "Camera discovery receive failed: errno %d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Camera discovery reply received +%lld ms after start",
                 (long long)((esp_timer_get_time() - discovery_started_us) /
                             1000));

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


esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway)
{
    if (gateway == NULL || gateway->addr == 0) {
        ESP_LOGE(TAG, "Cannot start camera protocol without a valid gateway");
        return ESP_ERR_INVALID_ARG;
    }
    s_camera_gateway = *gateway;
    s_camera_gateway_valid = true;
    return discover_camera();
}
