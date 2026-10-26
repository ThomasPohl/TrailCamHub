#include <errno.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "camera_udp_transport.h"

#define CAMERA_UDP_MAX_PACKET_LEN 512

static const char *TAG = "trailcamhub_udp";

static void format_packet_hex(const uint8_t *packet, size_t packet_len,
                              char *output, size_t output_size)
{
    size_t output_len = 0;
    if (output_size == 0) {
        return;
    }
    output[0] = '\0';
    for (size_t i = 0; i < packet_len && output_len < output_size; i++) {
        int written = snprintf(output + output_len, output_size - output_len,
                               "%s%02X", i == 0 ? "" : " ", packet[i]);
        if (written < 0 || (size_t)written >= output_size - output_len) {
            output[output_len] = '\0';
            break;
        }
        output_len += (size_t)written;
    }
}

ssize_t camera_udp_send_packet(int socket_fd, const uint8_t *packet,
                               size_t packet_len,
                               const struct sockaddr_in *destination,
                               const char *context)
{
    ssize_t sent = sendto(socket_fd, packet, packet_len, 0,
                          (const struct sockaddr *)destination,
                          sizeof(*destination));
    esp_ip4_addr_t destination_ip = {.addr = destination->sin_addr.s_addr};
    if (sent < 0 || (size_t)sent != packet_len) {
        ESP_LOGE(TAG, "UDP TX [%s] to " IPSTR ":%u failed: errno %d",
                 context, IP2STR(&destination_ip),
                 ntohs(destination->sin_port), sent < 0 ? errno : EIO);
        return sent < 0 ? sent : -1;
    }

    char packet_hex[CAMERA_UDP_MAX_PACKET_LEN * 3 + 1];
    format_packet_hex(packet, packet_len, packet_hex, sizeof(packet_hex));
    ESP_LOGI(TAG, "UDP TX [%s] to " IPSTR ":%u (%u bytes): %s",
             context, IP2STR(&destination_ip), ntohs(destination->sin_port),
             (unsigned)sent, packet_hex);
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
    char packet_hex[CAMERA_UDP_MAX_PACKET_LEN * 3 + 1];
    format_packet_hex(packet, (size_t)received, packet_hex,
                      sizeof(packet_hex));
    ESP_LOGI(TAG, "UDP RX [%s] from " IPSTR ":%u (%u bytes): %s",
             context, IP2STR(&source_ip), ntohs(source->sin_port),
             (unsigned)received, packet_hex);
    return received;
}
