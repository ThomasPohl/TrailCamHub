#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "lwip/sockets.h"

ssize_t camera_udp_send_packet(int socket_fd, const uint8_t *packet,
                               size_t packet_len,
                               const struct sockaddr_in *destination,
                               const char *context);
ssize_t camera_udp_receive_packet(int socket_fd, uint8_t *packet,
                                  size_t packet_capacity,
                                  struct sockaddr_in *source,
                                  const char *context);
