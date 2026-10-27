#include <atomic>
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

#include "camera_prelogin.hpp"
#include "camera_protocol.h"
#include "camera_udp_transport.h"
#include "camera_udp_packets.hpp"
#include "pppp_session.hpp"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000
#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500

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
// Set by camera_protocol_request_stop(); checked at least every receive
// timeout (at most CAMERA_RUDP_RECEIVE_TIMEOUT_MS) by the session loops.
static std::atomic<bool> s_stop_requested;

static bool stop_requested(void)
{
    if (!s_stop_requested.load()) {
        return false;
    }
    ESP_LOGI(TAG, "Camera session stop requested");
    return true;
}

static esp_err_t build_prelogin_request(camera::PreLoginRequest *request)
{
    static const char *TAG_CRYPTO = "trailcamhub_wifi";
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t plaintext[96];
    uint8_t nonce[camera::kPreLoginNonceSize];
    uint8_t ciphertext[96];
    size_t json_len = 0;
    time_t utc_time = time(NULL);

    esp_fill_random(nonce, sizeof(nonce));
    size_t ciphertext_len = camera::build_prelogin_plaintext(
        utc_time, nonce, plaintext, sizeof(plaintext), &json_len);
    if (ciphertext_len == 0) {
        ESP_LOGE(TAG_CRYPTO, "Could not format PRE_LOGIN JSON payload");
        return ESP_ERR_INVALID_SIZE;
    }
    if (utc_time < 1700000000) {
        ESP_LOGW(TAG_CRYPTO,
                 "System UTC time appears unset (%lld); "
                 "PRE_LOGIN timestamp may be rejected",
                 (long long)utc_time);
    }
    if (ciphertext_len > sizeof(ciphertext) ||
        sizeof(s_prelogin_static_header) + ciphertext_len > UINT16_MAX) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN payload exceeds packet capacity");
        memset(plaintext, 0, sizeof(plaintext));
        return ESP_ERR_INVALID_SIZE;
    }

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

// Sets the socket receive timeout. lwIP converts SO_RCVTIMEO to whole
// milliseconds and treats 0 as "wait forever", so the timeout is rounded up
// and never shorter than 1 ms.
static esp_err_t set_receive_timeout(int socket_fd, int64_t timeout_us,
                                     const char *context)
{
    int64_t timeout_ms = (timeout_us + 999) / 1000;
    if (timeout_ms < 1) {
        timeout_ms = 1;
    }
    struct timeval timeout = {};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set %s receive timeout: errno %d", context,
                 errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Logs a packet that is not handled in the current state, naming its type.
static void log_ignored_packet(const char *state, const uint8_t *packet,
                               size_t packet_len)
{
    if (packet_len >= 2 && packet[0] == 0xf1) {
        ESP_LOGW(TAG, "Ignoring %u-byte packet type 0x%02X while %s",
                 (unsigned)packet_len, packet[1], state);
    } else {
        ESP_LOGW(TAG, "Ignoring %u-byte packet without F1 prefix while %s",
                 (unsigned)packet_len, state);
    }
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

// Sends session packets to the camera over the bound UDP socket.
class UdpTransport final : public camera::pppp::Transport
{
  public:
    void set_destination(int socket_fd, const struct sockaddr_in &destination)
    {
        socket_fd_ = socket_fd;
        destination_ = destination;
    }

    bool send(const uint8_t *data, size_t size, const char *context) override
    {
        ssize_t sent = camera_udp_send_packet(socket_fd_, data, size,
                                              &destination_, context);
        return sent >= 0 && static_cast<size_t>(sent) == size;
    }

  private:
    int socket_fd_ = -1;
    struct sockaddr_in destination_ = {};
};

using SessionEvent = camera::pppp::Session::Event;

static UdpTransport s_transport;
// Static because it holds the DATA packets awaiting acknowledgement.
static camera::pppp::Session s_session(s_transport);

static bool from_camera(const struct sockaddr_in *source,
                        const struct sockaddr_in *camera_address)
{
    return source->sin_addr.s_addr == camera_address->sin_addr.s_addr &&
           source->sin_port == camera_address->sin_port;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void log_received_data(const SessionEvent &event)
{
    static const uint8_t kArtemisSignature[] = {'A', 'R', 'T', 'E',
                                                'M', 'I', 'S', 0};
    if (event.payload_size >= 20 && memcmp(event.payload, kArtemisSignature,
                                           sizeof(kArtemisSignature)) == 0) {
        ESP_LOGI(TAG,
                 "Received ARTEMIS DATA index %u: message type %lu, "
                 "sequence %lu, %lu payload bytes",
                 event.index, (unsigned long)read_le32(event.payload + 8),
                 (unsigned long)read_le32(event.payload + 12),
                 (unsigned long)read_le32(event.payload + 16));
    } else {
        ESP_LOGI(TAG, "Received DATA index %u (%u bytes)", event.index,
                 (unsigned)event.payload_size);
    }
}

// Logs session events shared by all phases. Returns true if the camera
// closed the session.
static bool log_session_event(const SessionEvent &event, const uint8_t *packet,
                              size_t packet_len, const char *state)
{
    switch (event.type) {
    case SessionEvent::Type::data:
        log_received_data(event);
        break;
    case SessionEvent::Type::duplicate_data:
        ESP_LOGD(TAG, "Acknowledged repeated DATA index %u", event.index);
        break;
    case SessionEvent::Type::ack:
        ESP_LOGD(TAG, "Camera acknowledged index %u", event.index);
        break;
    case SessionEvent::Type::alive:
        ESP_LOGI(TAG, "Answered camera ALIVE");
        break;
    case SessionEvent::Type::close:
        ESP_LOGW(TAG, "Camera closed the session (CLOSE)");
        return true;
    case SessionEvent::Type::discovery_reply:
        ESP_LOGD(TAG, "Ignoring repeated camera discovery reply");
        break;
    case SessionEvent::Type::unhandled:
        log_ignored_packet(state, packet, packet_len);
        break;
    }
    return false;
}

static esp_err_t
send_artemis_handshake(int socket_fd, const struct sockaddr_in *camera_address)
{
    uint16_t hello_index = 0;
    if (!s_session.send_data(s_artemis_hello_body, sizeof(s_artemis_hello_body),
                             esp_timer_get_time(), "ARTEMIS Hello",
                             &hello_index)) {
        ESP_LOGE(TAG, "Could not send ARTEMIS Hello");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ARTEMIS Hello sent with index %u", hello_index);
    vTaskDelay(pdMS_TO_TICKS(50));

    camera::ArtemisMagicRequest magic_one;
    if (!magic_one.build(camera::ArtemisMagicRequest::Stage::first)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 1 request");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = send_udp_request(socket_fd, camera_address,
                                       "ARTEMIS Magic 1", magic_one);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    camera::ArtemisMagicRequest magic_two;
    if (!magic_two.build(camera::ArtemisMagicRequest::Stage::second)) {
        ESP_LOGE(TAG, "Could not build ARTEMIS Magic 2 request");
        return ESP_ERR_INVALID_SIZE;
    }
    return send_udp_request(socket_fd, camera_address, "ARTEMIS Magic 2",
                            magic_two);
}

// Keeps the ARTEMIS session alive with heartbeats (one at a time, sent
// after the previous one was acknowledged) and logs everything received
// until the camera closes the session or a stop is requested.
static esp_err_t
wait_for_artemis_packets(int socket_fd,
                         const struct sockaddr_in *camera_address,
                         int64_t discovery_started_us)
{
    camera::ArtemisHeartbeatPayload heartbeat;
    uint16_t heartbeat_index = 0;
    bool heartbeat_pending = false;
    int64_t last_heartbeat_us = 0;
    const int64_t heartbeat_interval_us =
        CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL;
    ESP_LOGI(TAG, "Starting ARTEMIS heartbeat loop");

    while (!stop_requested()) {
        int64_t now_us = esp_timer_get_time();
        if (!heartbeat_pending &&
            (last_heartbeat_us == 0 ||
             now_us - last_heartbeat_us >= heartbeat_interval_us)) {
            const uint8_t *body = heartbeat.next_command();
            if (s_session.send_data(body, heartbeat.size(), now_us,
                                    "ARTEMIS heartbeat", &heartbeat_index)) {
                heartbeat_pending = true;
                last_heartbeat_us = now_us;
                ESP_LOGI(TAG,
                         "ARTEMIS heartbeat command %u, index %u awaiting ACK",
                         heartbeat.command_count(), heartbeat_index);
            } else if (!s_session.transport_failed()) {
                ESP_LOGW(TAG, "No free DATA slot for the heartbeat");
            }
        }
        size_t retransmitted = s_session.poll(now_us);
        if (retransmitted > 0) {
            ESP_LOGW(TAG, "Retransmitted %u unacknowledged DATA packet(s)",
                     (unsigned)retransmitted);
        }
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }

        // Wake up in time for the next retransmission or heartbeat.
        int64_t timeout_us = s_session.time_until_retransmit_us(
            now_us, CAMERA_RUDP_RECEIVE_TIMEOUT_MS * 1000LL);
        if (!heartbeat_pending) {
            int64_t heartbeat_due_us =
                heartbeat_interval_us - (now_us - last_heartbeat_us);
            if (heartbeat_due_us < timeout_us) {
                timeout_us = heartbeat_due_us;
            }
        }
        esp_err_t timeout_error =
            set_receive_timeout(socket_fd, timeout_us, "ARTEMIS");
        if (timeout_error != ESP_OK) {
            return timeout_error;
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
        if (!from_camera(&source, camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }
        ESP_LOGD(
            TAG, "ARTEMIS packet received +%lld ms since discovery",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        const SessionEvent event =
            s_session.handle_packet(packet, static_cast<size_t>(received));
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }
        if (event.type == SessionEvent::Type::ack && heartbeat_pending &&
            !s_session.is_outstanding(heartbeat_index)) {
            ESP_LOGI(TAG, "Heartbeat index %u acknowledged", heartbeat_index);
            heartbeat_pending = false;
        } else if (log_session_event(event, packet,
                                     static_cast<size_t>(received),
                                     "in the ARTEMIS session")) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
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

    s_transport.set_destination(socket_fd, *camera_address);
    s_session.reset();
    error = send_udp_request(socket_fd, camera_address,
                             "AES-encrypted PRE_LOGIN", request);
    if (error != ESP_OK) {
        return error;
    }
    int64_t sent_at_us = esp_timer_get_time();

    // Wait for the camera's first DATA packet, which confirms PRE_LOGIN.
    int64_t response_deadline_us =
        sent_at_us + CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS * 1000;
    while (!stop_requested()) {
        int64_t remaining_us = response_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            ESP_LOGW(TAG,
                     "No DATA packet received within %u ms after "
                     "PRE_LOGIN; handshake is not verified",
                     CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        error = set_receive_timeout(socket_fd, remaining_us, "PRE_LOGIN");
        if (error != ESP_OK) {
            return error;
        }

        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            socket_fd, response, sizeof(response), &source, "PRE_LOGIN");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
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
        if (!from_camera(&source, camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }

        const SessionEvent event =
            s_session.handle_packet(response, static_cast<size_t>(received));
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }
        if (log_session_event(event, response, static_cast<size_t>(received),
                              "waiting for the PRE_LOGIN response")) {
            return ESP_ERR_INVALID_STATE;
        }
        if (event.type == SessionEvent::Type::data) {
            error = send_artemis_handshake(socket_fd, &source);
            if (error != ESP_OK) {
                return error;
            }
            return wait_for_artemis_packets(socket_fd, &source,
                                            discovery_started_us);
        }
    }
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

    if (set_receive_timeout(socket_fd,
                            CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS * 1000LL,
                            "camera discovery") != ESP_OK) {
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

    while (!stop_requested() &&
           (esp_timer_get_time() - discovery_started_us) / 1000 <
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
    if (s_stop_requested.load()) {
        return ESP_OK;
    }
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
    s_stop_requested = false;
    return discover_camera();
}

void camera_protocol_request_stop(void)
{
    s_stop_requested = true;
}
