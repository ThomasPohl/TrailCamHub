#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Starts the board's Ethernet (the on-chip MAC with an IP101 PHY on the
 * ESP32-P4-NANO, or a W5500 on SPI) and requests an
 * address over DHCP. Link and address changes are logged. The interface has
 * a higher route priority than the camera Wi-Fi, so it stays the default
 * route while the camera network is joined; the camera remains reachable
 * through its own subnet.
 *
 * Requires esp_netif_init() and the default event loop.
 */
esp_err_t ethernet_uplink_start(void);

/* True while the Ethernet interface has an IP address. */
bool ethernet_uplink_has_ip(void);

/*
 * Called from the default event loop when the interface gets (true) or loses
 * (false) its IP address. One callback; NULL removes it.
 */
void ethernet_uplink_set_ip_callback(void (*callback)(bool has_ip));
