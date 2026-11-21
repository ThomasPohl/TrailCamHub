#include "mock_udp.hpp"

#include <algorithm>
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

extern const uint8_t thumbnail_start[] asm("_binary_thumbnail_jpg_start");
extern const uint8_t thumbnail_end[] asm("_binary_thumbnail_jpg_end");
extern const uint8_t photo_start[] asm("_binary_photo_jpg_start");
extern const uint8_t photo_end[] asm("_binary_photo_jpg_end");

namespace mock_udp
{

namespace
{

const char *TAG = "mock_udp";
constexpr std::uint16_t kCameraPort = 40611;
// The vendor app broadcasts its LAN search (F1 30 00 00) to this port, to
// 192.168.43.255 and 255.255.255.255; the camera answers from kCameraPort.
constexpr std::uint16_t kLanSearchPort = 32108;
// Wait for packets at most this long, and so the resolution of the mock's
// timers.
constexpr int kPollMs = 10;
constexpr std::size_t kMaxDatagram = 1600;

// Sends to the hub (or app) that sent the last discovery or LAN search,
// always from the camera port.
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

int s_search_fd = -1;

bool is_type(const std::uint8_t *buffer, int size, std::uint8_t type)
{
    return size >= 2 && buffer[0] == camera_mock::wire::kMagic &&
           buffer[1] == type;
}

// A packet on the camera port: a discovery (0x41) from anyone, everything
// else only from the client of the session.
void handle_camera_port(const std::uint8_t *buffer, int received,
                        const struct sockaddr_in &source)
{
    const bool discovery =
        is_type(buffer, received, camera_mock::wire::kDiscoveryRequest);
    if (discovery) {
        if (!s_output.has_peer || !same_endpoint(s_output.peer, source)) {
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
        if (!s_camera->handle_packet(buffer, static_cast<std::size_t>(received),
                                     now_ms())) {
            ESP_LOGW(TAG, "Not a camera packet (%d bytes)", received);
        }
    } else {
        ESP_LOGW(TAG, "%d bytes from an unknown sender ignored", received);
    }
}

// A packet on the LAN search port: only the app's LAN search counts. The
// answer goes to the address and port it came from.
void handle_search_port(const std::uint8_t *buffer, int received,
                        const struct sockaddr_in &source)
{
    if (!is_type(buffer, received, camera_mock::wire::kLanSearch)) {
        ESP_LOGW(TAG, "%d bytes on port %u ignored", received, kLanSearchPort);
        return;
    }
    const bool same = s_output.has_peer && same_endpoint(s_output.peer, source);
    if (!same) {
        if (s_camera->state() == camera_mock::Camera::State::session) {
            // A new app session while an old one is still open: the old
            // client is gone or is replaced.
            ESP_LOGI(TAG, "LAN search from another client; the session is "
                          "closed");
            s_camera->close();
        }
        char address[16];
        inet_ntoa_r(source.sin_addr, address, sizeof(address));
        ESP_LOGI(TAG, "LAN search from %s:%u (vendor app)", address,
                 ntohs(source.sin_port));
        s_output.peer = source;
        s_output.has_peer = true;
    }
    s_camera->handle_packet(buffer, static_cast<std::size_t>(received),
                            now_ms());
}

// Reads every waiting datagram of one socket.
void drain(int fd, bool search_port, std::uint8_t *buffer, std::size_t size)
{
    while (true) {
        struct sockaddr_in source = {};
        socklen_t source_size = sizeof(source);
        const int received = recvfrom(
            fd, buffer, size, MSG_DONTWAIT,
            reinterpret_cast<struct sockaddr *>(&source), &source_size);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
            }
            return;
        }
        if (received == 0) {
            continue;
        }
        std::lock_guard<std::mutex> guard(s_lock);
        if (search_port) {
            handle_search_port(buffer, received, source);
        } else {
            handle_camera_port(buffer, received, source);
        }
    }
}

void udp_task(void *arg)
{
    (void)arg;
    static std::uint8_t buffer[kMaxDatagram];
    while (true) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s_output.fd, &readable);
        int max_fd = s_output.fd;
        if (s_search_fd >= 0) {
            FD_SET(s_search_fd, &readable);
            max_fd = std::max(max_fd, s_search_fd);
        }
        struct timeval timeout = {};
        timeout.tv_usec = kPollMs * 1000;
        const int ready =
            select(max_fd + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) {
            ESP_LOGW(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (ready > 0) {
            if (FD_ISSET(s_output.fd, &readable)) {
                drain(s_output.fd, false, buffer, sizeof(buffer));
            }
            if (s_search_fd >= 0 && FD_ISSET(s_search_fd, &readable)) {
                drain(s_search_fd, true, buffer, sizeof(buffer));
            }
        }
        std::lock_guard<std::mutex> guard(s_lock);
        s_camera->poll(now_ms());
    }
}

// A UDP socket bound to `port` on all addresses, broadcasts allowed.
int open_socket(std::uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) <
        0) {
        ESP_LOGE(TAG, "bind to port %u failed: errno %d", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

esp_err_t start(const camera_mock::Identity &identity)
{
    const int fd = open_socket(kCameraPort);
    if (fd < 0) {
        return ESP_FAIL;
    }
    // Without it the hub still works; only the vendor app does not find
    // the mock.
    s_search_fd = open_socket(kLanSearchPort);
    if (s_search_fd < 0) {
        ESP_LOGW(TAG, "No LAN search port; the vendor app will not find the "
                      "mock");
    }

    s_output.fd = fd;
    camera_mock::Options options;
    options.media_count = CONFIG_CAMERA_MOCK_MEDIA_COUNT;
    // The pictures embedded in the image (fixtures/make_images.py).
    options.thumbnail_jpeg = thumbnail_start;
    options.thumbnail_jpeg_size = thumbnail_end - thumbnail_start;
    options.photo_jpeg = photo_start;
    options.photo_jpeg_size = photo_end - photo_start;
    s_camera = new camera_mock::Camera(s_output, log_line, options);
    s_camera->set_identity(identity);
    if (xTaskCreate(udp_task, "mock_udp", 6144, nullptr, 5, nullptr) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Listening on UDP ports %u and %u (LAN search)", kCameraPort,
             kLanSearchPort);
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
        "LAN search %u, discovery %u, PRE_LOGIN %u, responses %u\n"
        "DATA in %u (duplicates %u, out of order %u), out %u (resent %u)\n"
        "ALIVE sent %u, ALIVE ACKs %u\n",
        state, stats.sessions, static_cast<unsigned>(s_camera->outstanding()),
        stats.lan_searches, stats.discoveries, stats.pre_logins,
        stats.responses, stats.data_received, stats.duplicates,
        stats.out_of_order, stats.data_sent, stats.retransmissions,
        stats.alive_sent, stats.alive_acks);
    std::string out = text;
    out += "requests:";
    for (const auto &entry : stats.requests) {
        out += " " + std::to_string(entry.first) + "x" +
               std::to_string(entry.second);
    }
    out += "\n";
    out += "faults: " + camera_mock::describe(s_camera->faults()) +
           " (dropped " + std::to_string(stats.dropped) + ", duplicated " +
           std::to_string(stats.duplicated) + ", unanswered " +
           std::to_string(stats.muted) + ")\n";
    out += "bulk: thumbnails " + std::to_string(stats.thumbnails) +
           ", photo downloads " + std::to_string(stats.downloads) +
           ", frames " + std::to_string(stats.bulk_frames_sent) + ", resent " +
           std::to_string(stats.bulk_retransmissions) + "\n";
    out += "actions: photos " + std::to_string(stats.photos) + ", deletions " +
           std::to_string(stats.deletions) + ", formats " +
           std::to_string(stats.formats) +
           (s_camera->recording() ? ", recording a video" : "") +
           (s_camera->formatted() ? ", card formatted (empty list)" : "") +
           "\n";
    return out;
}

camera_mock::Identity identity()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_camera != nullptr ? s_camera->identity() : camera_mock::Identity();
}

camera_mock::Faults faults()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_camera != nullptr ? s_camera->faults() : camera_mock::Faults();
}

void set_faults(const camera_mock::Faults &faults)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_camera != nullptr) {
        s_camera->set_faults(faults);
    }
}

size_t media_count()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_camera != nullptr ? s_camera->media_count() : 0;
}

void set_media_count(size_t count)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_camera != nullptr) {
        s_camera->set_media_count(count);
    }
}

void close_session()
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_camera != nullptr) {
        s_camera->close();
    }
}

}  // namespace mock_udp
