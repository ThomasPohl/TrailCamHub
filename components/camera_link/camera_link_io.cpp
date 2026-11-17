#include "camera_link_io.hpp"

#include <atomic>
#include <errno.h>
#include <string.h>

#include "esp_log.h"

#include "camera_udp_transport.h"

namespace camera::link
{

namespace
{

const char *TAG = "trailcamhub_protocol";

std::atomic<bool> s_stop_requested;

// The receive timeout last set, so that an unchanged one costs no system call
// (it is set before every received frame, about 14,000 times per video).
int s_timeout_socket = -1;
std::int64_t s_timeout_ms = -1;

std::uint32_t read_le32(const std::uint8_t *bytes)
{
    return (std::uint32_t)bytes[0] | ((std::uint32_t)bytes[1] << 8) |
           ((std::uint32_t)bytes[2] << 16) | ((std::uint32_t)bytes[3] << 24);
}

void log_received_data(const camera::pppp::Session::Event &event)
{
    static const std::uint8_t kArtemisSignature[] = {'A', 'R', 'T', 'E',
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

}  // namespace

void clear_stop()
{
    s_stop_requested = false;
}

void request_stop()
{
    s_stop_requested = true;
}

bool stop_requested()
{
    if (!s_stop_requested.load()) {
        return false;
    }
    ESP_LOGI(TAG, "Camera session stop requested");
    return true;
}

bool stop_pending()
{
    return s_stop_requested.load();
}

esp_err_t set_receive_timeout(int socket_fd, std::int64_t timeout_us,
                              const char *context)
{
    std::int64_t timeout_ms = (timeout_us + 999) / 1000;
    if (timeout_ms < 1) {
        timeout_ms = 1;
    }
    if (socket_fd == s_timeout_socket && timeout_ms == s_timeout_ms) {
        return ESP_OK;
    }
    struct timeval timeout = {};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
    timeout.tv_usec =
        static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set %s receive timeout: errno %d", context,
                 errno);
        s_timeout_socket = -1;
        return ESP_FAIL;
    }
    s_timeout_socket = socket_fd;
    s_timeout_ms = timeout_ms;
    return ESP_OK;
}

void forget_receive_timeout()
{
    s_timeout_socket = -1;
}

esp_err_t send_udp_request(int socket_fd, const struct sockaddr_in *destination,
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

bool from_camera(const struct sockaddr_in *source,
                 const struct sockaddr_in *camera_address)
{
    return source->sin_addr.s_addr == camera_address->sin_addr.s_addr &&
           source->sin_port == camera_address->sin_port;
}

void log_ignored_packet(const char *state, const std::uint8_t *packet,
                        std::size_t packet_len)
{
    if (packet_len >= 2 && packet[0] == 0xf1) {
        ESP_LOGW(TAG, "Ignoring %u-byte packet type 0x%02X while %s",
                 (unsigned)packet_len, packet[1], state);
    } else {
        ESP_LOGW(TAG, "Ignoring %u-byte packet without F1 prefix while %s",
                 (unsigned)packet_len, state);
    }
}

bool log_session_event(const camera::pppp::Session::Event &event,
                       const std::uint8_t *packet, std::size_t packet_len,
                       const char *state)
{
    using Type = camera::pppp::Session::Event::Type;
    switch (event.type) {
    case Type::data:
        log_received_data(event);
        break;
    case Type::duplicate_data:
        ESP_LOGD(TAG, "Acknowledged repeated DATA index %u", event.index);
        break;
    case Type::early_data:
        ESP_LOGD(TAG, "DATA index %u after a gap, waiting for the missing one",
                 event.index);
        break;
    case Type::ack:
        ESP_LOGD(TAG, "Camera acknowledged index %u", event.index);
        break;
    case Type::alive:
        ESP_LOGI(TAG, "Answered camera ALIVE");
        break;
    case Type::close:
        ESP_LOGW(TAG, "Camera closed the session (CLOSE)");
        return true;
    case Type::discovery_reply:
        ESP_LOGD(TAG, "Ignoring repeated camera discovery reply");
        break;
    case Type::unhandled:
        log_ignored_packet(state, packet, packet_len);
        break;
    }
    return false;
}

void UdpTransport::set_destination(int socket_fd,
                                   const struct sockaddr_in &destination)
{
    socket_fd_ = socket_fd;
    destination_ = destination;
}

bool UdpTransport::send(const std::uint8_t *data, std::size_t size,
                        const char *context)
{
    ssize_t sent =
        camera_udp_send_packet(socket_fd_, data, size, &destination_, context);
    return sent >= 0 && static_cast<size_t>(sent) == size;
}

}  // namespace camera::link
