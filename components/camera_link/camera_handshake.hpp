#pragma once

// Handshake with the camera after joining its Wi-Fi: LBCS discovery to the
// gateway, PRE_LOGIN, and the ARTEMIS login (Hello) with the two Magic
// packets. The session that follows runs in camera_protocol.cpp. ESP-IDF
// only.

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "lwip/sockets.h"

#include "artemis_aes128.hpp"
#include "camera_link_io.hpp"
#include "pppp_session.hpp"

namespace camera::link
{

// Opens and binds the camera UDP socket, sends the discovery to `gateway`
// and waits for the camera's reply. `discovery_started_us` is when the
// handshake started (esp_timer_get_time()); the timeout counts from it.
//
// On ESP_OK, `*socket_fd` is the open socket (the caller closes it) and
// `*camera_address` the endpoint that answered, unless a stop was requested:
// then `*socket_fd` is -1. On an error no socket is left open.
esp_err_t discover_camera(const esp_ip4_addr_t &gateway,
                          std::int64_t discovery_started_us, int *socket_fd,
                          struct sockaddr_in *camera_address);

// Sends PRE_LOGIN to `camera_address` and waits for the camera's first DATA
// packet, which confirms it. `session` is reset and handles the packets
// (`transport` is pointed at the camera first). On ESP_OK, `*confirmed` tells
// whether the DATA packet came (false: a stop was requested) and
// `*data_source` is its endpoint.
esp_err_t exchange_prelogin(int socket_fd,
                            const struct sockaddr_in &camera_address,
                            UdpTransport &transport,
                            camera::pppp::Session &session,
                            std::int64_t discovery_started_us,
                            struct sockaddr_in *data_source, bool *confirmed);

// Sends the ARTEMIS login (Hello, cmdId 0) over `session`, then the two
// Magic packets.
esp_err_t send_login(int socket_fd, const struct sockaddr_in &camera_address,
                     camera::pppp::Session &session,
                     const camera::artemis::Aes128 &cipher,
                     const std::string &user, const std::string &password);

}  // namespace camera::link
