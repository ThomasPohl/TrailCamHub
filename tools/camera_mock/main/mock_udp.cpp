#include "mock_udp.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "mock_camera.hpp"
#include "mock_wire.hpp"

namespace mock_udp
{

namespace
{

const char *TAG = "mock_udp";
constexpr std::uint16_t kCameraPort = 40611;
// Receive timeout, and so the resolution of the mock's timers.
constexpr int kPollMs = 10;
constexpr std::size_t kMaxDatagram = 1600;

// Sends to the hub that sent the last discovery.
class UdpOutput final : public camera_mock::Output
{
  public:
    int fd = -1;
    struct sockaddr_in peer = {};
    bool has_peer = false;

    void send(const std::uint8_t *data, std::size_t size) override
    {
        if (!has_peer) {
            return;
        }
        const int sent = sendto(
            fd, data, size, 0, reinterpret_cast<const struct sockaddr *>(&peer),
            sizeof(peer));
        if (sent < 0) {
            ESP_LOGW(TAG, "sendto failed: errno %d", errno);
        }
    }
};

void log_line(camera_mock::LogLevel level, const std::string &text)
{
    if (level == camera_mock::LogLevel::warn) {
        ESP_LOGW(TAG, "%s", text.c_str());
    } else {
        ESP_LOGI(TAG, "%s", text.c_str());
    }
}

std::mutex s_lock;
UdpOutput s_output;
camera_mock::Camera *s_camera;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool same_endpoint(const struct sockaddr_in &a, const struct sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void udp_task(void *arg)
{
    (void)arg;
    static std::uint8_t buffer[kMaxDatagram];
    while (true) {
        struct sockaddr_in source = {};
        socklen_t source_size = sizeof(source);
        const int received = recvfrom(
            s_output.fd, buffer, sizeof(buffer), 0,
            reinterpret_cast<struct sockaddr *>(&source), &source_size);

        std::lock_guard<std::mutex> guard(s_lock);
        if (received > 0) {
            const bool discovery =
                received >= 2 && buffer[0] == camera_mock::wire::kMagic &&
                buffer[1] == camera_mock::wire::kDiscoveryRequest;
            if (discovery) {
                if (!s_output.has_peer ||
                    !same_endpoint(s_output.peer, source)) {
                    char address[16];
                    inet_ntoa_r(source.sin_addr, address, sizeof(address));
                    ESP_LOGI(TAG, "Discovery from %s:%u", address,
                             ntohs(source.sin_port));
                }
                s_output.peer = source;
                s_output.has_peer = true;
            }
            if (discovery ||
                (s_output.has_peer && same_endpoint(s_output.peer, source))) {
                if (!s_camera->handle_packet(
                        buffer, static_cast<std::size_t>(received), now_ms())) {
                    ESP_LOGW(TAG, "Not a camera packet (%d bytes)", received);
                }
            } else {
                ESP_LOGW(TAG, "%d bytes from an unknown sender ignored",
                         received);
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_camera->poll(now_ms());
    }
}

}  // namespace

esp_err_t start()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return ESP_FAIL;
    }
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kCameraPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) <
        0) {
        ESP_LOGE(TAG, "bind to port %u failed: errno %d", kCameraPort, errno);
        close(fd);
        return ESP_FAIL;
    }
    struct timeval timeout = {};
    timeout.tv_usec = kPollMs * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    s_output.fd = fd;
    s_camera = new camera_mock::Camera(s_output, log_line);
    if (xTaskCreate(udp_task, "mock_udp", 6144, nullptr, 5, nullptr) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Listening on UDP port %u", kCameraPort);
    return ESP_OK;
}

std::string status()
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_camera == nullptr) {
        return "UDP not started\n";
    }
    const camera_mock::Stats &stats = s_camera->stats();
    const char *state = "idle";
    switch (s_camera->state()) {
    case camera_mock::Camera::State::idle:
        break;
    case camera_mock::Camera::State::discovered:
        state = "discovered";
        break;
    case camera_mock::Camera::State::session:
        state = s_camera->logged_in() ? "session, logged in" : "session";
        break;
    }
    char text[512];
    std::snprintf(
        text, sizeof(text),
        "session: %s, %u started, %u DATA unacknowledged\n"
        "discovery %u, PRE_LOGIN %u, responses %u\n"
        "DATA in %u (duplicates %u, out of order %u), out %u (resent %u)\n"
        "ALIVE sent %u, ALIVE ACKs %u\n",
        state, stats.sessions, static_cast<unsigned>(s_camera->outstanding()),
        stats.discoveries, stats.pre_logins, stats.responses,
        stats.data_received, stats.duplicates, stats.out_of_order,
        stats.data_sent, stats.retransmissions, stats.alive_sent,
        stats.alive_acks);
    std::string out = text;
    out += "requests:";
    for (const auto &entry : stats.requests) {
        out += " " + std::to_string(entry.first) + "x" +
               std::to_string(entry.second);
    }
    out += "\n";
    return out;
}

void close_session()
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_camera != nullptr) {
        s_camera->close();
    }
}

}  // namespace mock_udp
