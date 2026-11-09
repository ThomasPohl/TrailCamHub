#include "camera_handshake.hpp"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "artemis_message.hpp"
#include "camera_clock.hpp"
#include "camera_prelogin.hpp"
#include "camera_udp_packets.hpp"
#include "camera_udp_transport.h"
#include "time_sync.h"

#define CAMERA_DISCOVERY_LOCAL_PORT 35281
#define CAMERA_DISCOVERY_TIMEOUT_MS 1500
#define CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS 500
#define CAMERA_PRELOGIN_RECEIVE_TIMEOUT_MS 1000

namespace camera::link
{

namespace
{

const char *TAG = "trailcamhub_protocol";
const uint16_t s_camera_discovery_ports[] = {40611, 3333};

using SessionEvent = camera::pppp::Session::Event;

esp_err_t build_prelogin_request(camera::PreLoginRequest *request)
{
    static const char *TAG_CRYPTO = "trailcamhub_wifi";
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t nonce[camera::kPreLoginNonceSize];
    uint8_t body[camera::kPreLoginMaxBodySize];
    size_t json_len = 0;
    time_t utc_time = time(NULL);

    esp_fill_random(nonce, sizeof(nonce));
    const size_t body_len = camera::build_prelogin_body(
        utc_time, nonce, body, sizeof(body), &json_len);
    if (body_len == 0) {
        ESP_LOGE(TAG_CRYPTO, "Could not format PRE_LOGIN JSON payload");
        return ESP_ERR_INVALID_SIZE;
    }
    if (utc_time < 1700000000) {
        ESP_LOGW(TAG_CRYPTO,
                 "System UTC time appears unset (%lld); "
                 "PRE_LOGIN timestamp may be rejected",
                 (long long)utc_time);
    }
    bool built = request->build(body, body_len);
    memset(body, 0, sizeof(body));
    if (!built) {
        ESP_LOGE(TAG_CRYPTO, "PRE_LOGIN packet exceeds supported size");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGD(TAG_CRYPTO,
             "Built PRE_LOGIN with %u-byte JSON, %u-byte "
             "ciphertext, UTC timestamp %lld",
             (unsigned)json_len,
             (unsigned)(body_len - camera::kPreLoginHeaderSize),
             (long long)utc_time);
    return ESP_OK;
}

}  // namespace

esp_err_t discover_camera(const esp_ip4_addr_t &gateway,
                          std::int64_t discovery_started_us, int *socket_fd,
                          struct sockaddr_in *camera_address)
{
    *socket_fd = -1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    forget_receive_timeout();
    if (fd < 0) {
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
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG,
                 "Could not bind camera UDP socket to local port %u: "
                 "errno %d",
                 CAMERA_DISCOVERY_LOCAL_PORT, errno);
        close(fd);
        return ESP_FAIL;
    }

    if (set_receive_timeout(fd, CAMERA_DISCOVERY_RECEIVE_TIMEOUT_MS * 1000LL,
                            "camera discovery") != ESP_OK) {
        close(fd);
        return ESP_FAIL;
    }

    const camera::CameraDiscoveryRequest discovery_packet;
    ESP_LOGI(TAG,
             "Camera discovery target " IPSTR ", source UDP port %u, "
             "payload %u bytes",
             IP2STR(&gateway), CAMERA_DISCOVERY_LOCAL_PORT,
             static_cast<unsigned>(discovery_packet.size()));

    for (size_t port_index = 0;
         port_index <
         sizeof(s_camera_discovery_ports) / sizeof(s_camera_discovery_ports[0]);
         port_index++) {
        struct sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(s_camera_discovery_ports[port_index]);
        destination.sin_addr.s_addr = gateway.addr;
        esp_err_t error = send_udp_request(fd, &destination, "LBCS discovery",
                                           discovery_packet);
        if (error != ESP_OK) {
            close(fd);
            return ESP_FAIL;
        }
    }

    while (!stop_requested() &&
           (esp_timer_get_time() - discovery_started_us) / 1000 <
               CAMERA_DISCOVERY_TIMEOUT_MS) {
        uint8_t response[256];
        struct sockaddr_in source = {};
        ssize_t received = camera_udp_receive_packet(
            fd, response, sizeof(response), &source, "camera discovery");
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "Camera discovery receive failed: errno %d", errno);
            close(fd);
            return ESP_FAIL;
        }

        ESP_LOGI(
            TAG, "Camera discovery reply received +%lld ms after start",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        if (received > 4 && response[0] == 0xf1 &&
            source.sin_addr.s_addr == gateway.addr) {
            ESP_LOGI(TAG,
                     "Camera UDP response accepted; using source port "
                     "%u for PRE_LOGIN",
                     ntohs(source.sin_port));
            *socket_fd = fd;
            *camera_address = source;
            return ESP_OK;
        }
        ESP_LOGW(TAG,
                 "UDP packet did not match camera response "
                 "(expected F1 prefix from gateway " IPSTR ")",
                 IP2STR(&gateway));
    }

    close(fd);
    if (stop_pending()) {
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "No matching camera discovery response after %lld ms "
             "(target " IPSTR ", source UDP port %u)",
             (long long)((esp_timer_get_time() - discovery_started_us) / 1000),
             IP2STR(&gateway), CAMERA_DISCOVERY_LOCAL_PORT);
    return ESP_ERR_TIMEOUT;
}

esp_err_t exchange_prelogin(int socket_fd,
                            const struct sockaddr_in &camera_address,
                            UdpTransport &transport,
                            camera::pppp::Session &session,
                            std::int64_t discovery_started_us,
                            struct sockaddr_in *data_source, bool *confirmed)
{
    *confirmed = false;
    camera::PreLoginRequest request;
    esp_err_t error = build_prelogin_request(&request);
    if (error != ESP_OK) {
        return error;
    }

    transport.set_destination(socket_fd, camera_address);
    session.reset();
#if CONFIG_CAMERA_SEND_PRELOGIN
    error = send_udp_request(socket_fd, &camera_address,
                             "AES-encrypted PRE_LOGIN", request);
    if (error != ESP_OK) {
        return error;
    }
#else
    ESP_LOGI(TAG, "PRE_LOGIN not sent (CONFIG_CAMERA_SEND_PRELOGIN off); "
                  "waiting for the camera's first DATA packet");
#endif
    int64_t sent_at_us = esp_timer_get_time();

    // Wait for the camera's first DATA packet. The app's captures show it
    // after discovery without a PRE_LOGIN (#51), so it does not necessarily
    // confirm the PRE_LOGIN.
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
        if (!from_camera(&source, &camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }

        const SessionEvent event =
            session.handle_packet(response, static_cast<size_t>(received));
        if (session.transport_failed()) {
            return ESP_FAIL;
        }
        if (log_session_event(event, response, static_cast<size_t>(received),
                              "waiting for the PRE_LOGIN response")) {
            return ESP_ERR_INVALID_STATE;
        }
        if (event.type == SessionEvent::Type::data) {
            *data_source = source;
            *confirmed = true;
            return ESP_OK;
        }
    }
    return ESP_OK;
}

esp_err_t send_login(int socket_fd, const struct sockaddr_in &camera_address,
                     camera::pppp::Session &session,
                     const camera::artemis::Aes128 &cipher,
                     const std::string &user, const std::string &password)
{
    // The hello is the login (cmdId 0). The camera sets its clock from the
    // time in it, so it carries the current wall clock when the device has a
    // time that it can vouch for (SNTP, the backend or a browser). Without
    // one the time is left out and the camera keeps its own: a fixed time
    // would set its clock back at every session, and a time that came from
    // the camera itself must not be given back to it.
    const time_t now = time(NULL);
    const bool send_time = time_sync_trusted();
    const int64_t camera_time = send_time ? camera::camera_wall_clock(now) : 0;
    if (send_time) {
        ESP_LOGI(TAG, "Login sets the camera clock to %lld (UTC %lld)",
                 (long long)camera_time, (long long)now);
    } else if (time_sync_clock_valid()) {
        ESP_LOGW(TAG, "The clock comes from the camera; login without a time");
    } else {
        ESP_LOGW(TAG, "The clock is not set; login without a time, the camera "
                      "keeps its own clock");
    }
    const std::vector<uint8_t> hello = camera::artemis::build_request(
        cipher, 1,
        camera::artemis::build_login_json(camera_time, user, password));
    uint16_t hello_index = 0;
    if (hello.empty() ||
        !session.send_data(hello.data(), hello.size(), esp_timer_get_time(),
                           "ARTEMIS Hello", &hello_index)) {
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
    esp_err_t error = send_udp_request(socket_fd, &camera_address,
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
    return send_udp_request(socket_fd, &camera_address, "ARTEMIS Magic 2",
                            magic_two);
}

}  // namespace camera::link
