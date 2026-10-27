#include <errno.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "camera_udp_transport.h"

static const char *TAG = "trailcamhub_udp";

static void log_packet_contents(const uint8_t *packet, size_t packet_len)
{
#if CONFIG_CAMERA_UDP_LOG_PACKET_HEX
    // Streams the packet in 16-byte lines through a small internal buffer
    // instead of formatting the whole packet on the caller's stack.
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, packet, packet_len, ESP_LOG_INFO);
#else
    (void)packet;
    (void)packet_len;
#endif
}

ssize_t camera_udp_send_packet(int socket_fd, const uint8_t *packet,
                               size_t packet_len,
                               const struct sockaddr_in *destination,
                               const char *context)
{
    ssize_t sent =
        sendto(socket_fd, packet, packet_len, 0,
               (const struct sockaddr *)destination, sizeof(*destination));
    esp_ip4_addr_t destination_ip = {.addr = destination->sin_addr.s_addr};
    if (sent < 0 || (size_t)sent != packet_len) {
        ESP_LOGE(TAG, "UDP TX [%s] to " IPSTR ":%u failed: errno %d", context,
                 IP2STR(&destination_ip), ntohs(destination->sin_port),
                 sent < 0 ? errno : EIO);
        return sent < 0 ? sent : -1;
    }

    ESP_LOGI(TAG, "UDP TX [%s] to " IPSTR ":%u (%u bytes)", context,
             IP2STR(&destination_ip), ntohs(destination->sin_port),
             (unsigned)sent);
    log_packet_contents(packet, packet_len);
    return sent;
}

ssize_t camera_udp_receive_packet(int socket_fd, uint8_t *packet,
                                  size_t packet_capacity,
                                  struct sockaddr_in *source,
                                  const char *context)
{
    socklen_t source_len = sizeof(*source);
    ssize_t received = recvfrom(socket_fd, packet, packet_capacity, 0,
                                (struct sockaddr *)source, &source_len);
    if (received < 0) {
        return received;
    }

    esp_ip4_addr_t source_ip = {.addr = source->sin_addr.s_addr};
    ESP_LOGI(TAG, "UDP RX [%s] from " IPSTR ":%u (%u bytes)", context,
             IP2STR(&source_ip), ntohs(source->sin_port), (unsigned)received);
    log_packet_contents(packet, (size_t)received);
    return received;
}
