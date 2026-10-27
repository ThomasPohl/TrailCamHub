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
#include "camera_udp_packets.hpp"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000
#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500
#define CAMERA_RUDP_RETRANSMIT_INTERVAL_MS 800

static const char *TAG = "trailcamhub_protocol";
static const uint16_t s_camera_discovery_ports[] = {40611, 3333};
// clang-format off
static const uint8_t s_prelogin_static_header[] = {
    0x0c, 0xcb, 0x9a, 0x2b, 0x5f, 0x95, 0x1e, 0xb6,
    0x69, 0xdf, 0xaa, 0x37, 0x5a, 0x6b, 0xbe, 0x3e,
    0x76, 0x20, 0x2e, 0x13, 0xc9, 0xd1, 0xaa, 0x36,
    0x31, 0xbe, 0x74, 0xe5,
};
// clang-format on
static const uint8_t s_prelogin_aes_key[16] = {
    'a', '0', '1', 'b', 'c', '2', '3', 'e',
    'd', '4', '5', 'f', 'F', '5', '6', 'A',
};
// clang-format off
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
// clang-format on
static esp_ip4_addr_t s_camera_gateway;
static bool s_camera_gateway_valid;
static uint8_t s_camera_rudp_sequence;

static esp_err_t build_prelogin_request(camera::PreLoginRequest *request)
{
    static const char *TAG_CRYPTO = "trailcamhub_wifi";
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
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
        ESP_LOGW(TAG_CRYPTO,
                 "System UTC time appears unset (%lld); "
                 "PRE_LOGIN timestamp may be rejected",
                 (long long)utc_time);
    }

    size_t padding_len = 16 - ((size_t)json_len % 16);
    ciphertext_len = (size_t)json_len + padding_len;
    if (ciphertext_len > sizeof(ciphertext) ||
        sizeof(s_prelogin_static_header) + ciphertext_len > UINT16_MAX) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN payload exceeds packet capacity");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(plaintext, json, (size_t)json_len);
    memset(plaintext + json_len, (int)padding_len, padding_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int mbedtls_error = mbedtls_aes_setkey_enc(&aes, s_prelogin_aes_key, 128);
    if (mbedtls_error == 0) {
        for (size_t offset = 0; offset < ciphertext_len; offset += 16) {
            mbedtls_error =
                mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                      plaintext + offset, ciphertext + offset);
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
    uint8_t body[sizeof(s_prelogin_static_header) + sizeof(ciphertext)];
    memcpy(body, s_prelogin_static_header, sizeof(s_prelogin_static_header));
    memcpy(body + sizeof(s_prelogin_static_header), ciphertext, ciphertext_len);
    bool built = request->build(body, body_len);
    memset(body, 0, body_len);
    memset(ciphertext, 0, sizeof(ciphertext));
    if (!built) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN packet exceeds supported size");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGD(TAG_CRYPTO,
             "Built PRE_LOGIN with %u-byte JSON, %u-byte "
             "ciphertext, UTC timestamp %lld",
             (unsigned)json_len, (unsigned)ciphertext_len, (long long)utc_time);
    return ESP_OK;
}

static uint8_t next_rudp_sequence(void)
{
    uint8_t sequence = (uint8_t)(s_camera_rudp_sequence % 254 + 1);
    s_camera_rudp_sequence = sequence;
    return sequence;
}

static esp_err_t send_udp_request(int socket_fd,
                                  const struct sockaddr_in *destination,
                                  const char *request_name,
                                  const camera::UdpRequest &request)
{
    ssize_t sent = camera_udp_send_packet(
        socket_fd, request.data(), request.size(), destination, request_name);
    if (sent < 0 || static_cast<size_t>(sent) != request.size()) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_data_ack(int socket_fd,
                               const struct sockaddr_in *camera_address,
                               uint8_t received_sequence)
{
    camera::ArtemisAckRequest request;
    uint8_t sequence = next_rudp_sequence();
    if (!request.build(sequence, received_sequence)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS DATA ACK request");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = send_udp_request(socket_fd, camera_address,
                                       "ARTEMIS DATA ACK", request);
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG,
             "ARTEMIS DATA ACK acknowledges received sequence %u "
             "using local sequence %u",
             received_sequence, sequence);
    return ESP_OK;
}

static esp_err_t
send_artemis_handshake(int socket_fd, const struct sockaddr_in *camera_address)
{
    camera::ArtemisDataRequest hello_request;
    if (!hello_request.build(next_rudp_sequence(), s_artemis_hello_body,
                             sizeof(s_artemis_hello_body))) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Hello request");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = send_udp_request(socket_fd, camera_address,
                                       "ARTEMIS Hello", hello_request);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    camera::ArtemisMagicRequest magic_one;
    if (!magic_one.build(next_rudp_sequence(),
                         camera::ArtemisMagicRequest::Stage::first)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 1 request");
        return ESP_ERR_INVALID_SIZE;
    }
    error = send_udp_request(socket_fd, camera_address, "ARTEMIS Magic 1",
                             magic_one);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    camera::ArtemisMagicRequest magic_two;
    if (!magic_two.build(next_rudp_sequence(),
                         camera::ArtemisMagicRequest::Stage::second)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 2 request");
        return ESP_ERR_INVALID_SIZE;
    }
    return send_udp_request(socket_fd, camera_address, "ARTEMIS Magic 2",
                            magic_two);
}

static esp_err_t
wait_for_artemis_packets(int socket_fd,
                         const struct sockaddr_in *camera_address,
                         int64_t discovery_started_us)
{
    camera::ArtemisHeartbeatPayload heartbeat;
    camera::ArtemisDataRequest pending_packet;
    uint8_t pending_sequence = 0;
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
            const uint8_t *heartbeat_body = heartbeat.next_command();
            pending_sequence = next_rudp_sequence();
            if (!pending_packet.build(pending_sequence, heartbeat_body,
                                      heartbeat.size())) {
                ESP_LOGE(TAG, "Could not build ARTEMIS heartbeat request");
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t error = send_udp_request(
                socket_fd, camera_address, "ARTEMIS heartbeat", pending_packet);
            if (error != ESP_OK) {
                return error;
            }
            waiting_for_ack = true;
            last_heartbeat_us = now_us;
            last_retry_us = now_us;
            ESP_LOGI(TAG,
                     "ARTEMIS heartbeat command %u, sequence %u "
                     "awaiting ACK",
                     heartbeat.command_count(), pending_sequence);
        } else if (waiting_for_ack &&
                   now_us - last_retry_us >=
                       CAMERA_RUDP_RETRANSMIT_INTERVAL_MS * 1000LL) {
            esp_err_t error = send_udp_request(
                socket_fd, camera_address, "ARTEMIS heartbeat retransmission",
                pending_packet);
            if (error != ESP_OK) {
                return error;
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
        struct sockaddr_in source = {};
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

        ESP_LOGI(
            TAG, "ARTEMIS packet received +%lld ms since discovery",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        camera::ArtemisRudpPacket received_packet;
        if (!received_packet.parse(packet, static_cast<size_t>(received))) {
            ESP_LOGW(TAG, "Ignoring malformed ARTEMIS packet");
            continue;
        }

        if (received_packet.is_data()) {
            uint8_t received_sequence = received_packet.sequence();
            ESP_LOGI(TAG, "Received ARTEMIS DATA packet, sequence %u",
                     received_sequence);
            esp_err_t error =
                send_data_ack(socket_fd, &source, received_sequence);
            if (error != ESP_OK) {
                return error;
            }
        } else if (received_packet.is_ack()) {
            bool acknowledges_pending =
                waiting_for_ack &&
                received_packet.acknowledges(pending_sequence);
            if (acknowledges_pending) {
                ESP_LOGI(TAG, "Heartbeat sequence %u acknowledged",
                         pending_sequence);
                waiting_for_ack = false;
            } else {
                ESP_LOGI(TAG,
                         "Received ARTEMIS ACK packet not matching "
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
    camera::PreLoginRequest request;
    esp_err_t error = build_prelogin_request(&request);
    if (error != ESP_OK) {
        return error;
    }

    error = send_udp_request(socket_fd, camera_address,
                             "AES-encrypted PRE_LOGIN", request);
    if (error != ESP_OK) {
        return error;
    }
    int64_t sent_at_us = esp_timer_get_time();

    struct timeval response_timeout = {
        .tv_sec = CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec = (CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS % 1000) * 1000,
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
            ESP_LOGW(TAG,
                     "No DATA packet received within %u ms after "
                     "PRE_LOGIN; handshake is not verified",
                     CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        struct timeval response_timeout = {};
        response_timeout.tv_sec =
            static_cast<decltype(response_timeout.tv_sec)>(remaining_us /
                                                           1000000);
        response_timeout.tv_usec =
            static_cast<decltype(response_timeout.tv_usec)>(remaining_us %
                                                            1000000);
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &response_timeout,
                       sizeof(response_timeout)) < 0) {
            ESP_LOGE(TAG, "Could not set PRE_LOGIN response timeout: errno %d",
                     errno);
            return ESP_FAIL;
        }

        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "PRE_LOGIN");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG,
                         "No DATA packet received within %u ms after "
                         "PRE_LOGIN; handshake is not verified",
                         CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
                return ESP_ERR_TIMEOUT;
            }
            ESP_LOGE(TAG, "PRE_LOGIN response receive failed: errno %d", errno);
            return ESP_FAIL;
        }

        int64_t received_at_us = esp_timer_get_time();
        ESP_LOGI(TAG,
                 "PRE_LOGIN response received +%lld ms after send / "
                 "+%lld ms since discovery",
                 (long long)((received_at_us - sent_at_us) / 1000),
                 (long long)((received_at_us - discovery_started_us) / 1000));

        bool from_camera =
            source.sin_addr.s_addr == camera_address->sin_addr.s_addr &&
            source.sin_port == camera_address->sin_port;
        camera::ArtemisRudpPacket response_packet;
        if (from_camera &&
            response_packet.parse(response, static_cast<size_t>(received)) &&
            response_packet.is_data()) {
            ESP_LOGI(TAG, "Received ARTEMIS DATA packet, sequence %u",
                     response_packet.sequence());
            esp_err_t ack_error =
                send_data_ack(socket_fd, &source, response_packet.sequence());
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

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(CAMERA_DISCOVERY_LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ESP_LOGI(TAG, "Binding camera UDP socket to local port %u",
             CAMERA_DISCOVERY_LOCAL_PORT);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG,
                 "Could not bind camera UDP socket to local port %u: "
                 "errno %d",
                 CAMERA_DISCOVERY_LOCAL_PORT, errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    struct timeval receive_timeout = {
        .tv_sec = CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec = (CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set camera discovery timeout: errno %d",
                 errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    const camera::CameraDiscoveryRequest discovery_packet;
    ESP_LOGI(TAG,
             "Camera discovery target " IPSTR ", source UDP port %u, "
             "payload %u bytes",
             IP2STR(&s_camera_gateway), CAMERA_DISCOVERY_LOCAL_PORT,
             static_cast<unsigned>(discovery_packet.size()));

    for (size_t port_index = 0;
         port_index <
         sizeof(s_camera_discovery_ports) / sizeof(s_camera_discovery_ports[0]);
         port_index++) {
        struct sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(s_camera_discovery_ports[port_index]);
        destination.sin_addr.s_addr = s_camera_gateway.addr;
        esp_err_t error = send_udp_request(socket_fd, &destination,
                                           "LBCS discovery", discovery_packet);
        if (error != ESP_OK) {
            close(socket_fd);
            return ESP_FAIL;
        }
    }

    while ((esp_timer_get_time() - discovery_started_us) / 1000 <
           CAMERA_DISCOVERY_TIMEOUT_MS) {
        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "camera discovery");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "Camera discovery receive failed: errno %d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }

        ESP_LOGI(
            TAG, "Camera discovery reply received +%lld ms after start",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        if (received > 4 && response[0] == 0xf1 &&
            source.sin_addr.s_addr == s_camera_gateway.addr) {
            ESP_LOGI(TAG,
                     "Camera UDP response accepted; using source port "
                     "%u for PRE_LOGIN",
                     ntohs(source.sin_port));
            esp_err_t prelogin_error =
                send_prelogin(socket_fd, &source, discovery_started_us);
            close(socket_fd);
            return prelogin_error;
        }
        ESP_LOGW(TAG,
                 "UDP packet did not match camera response "
                 "(expected F1 prefix from gateway " IPSTR ")",
                 IP2STR(&s_camera_gateway));
    }

    close(socket_fd);
    ESP_LOGW(TAG,
             "No matching camera discovery response after %lld ms "
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
