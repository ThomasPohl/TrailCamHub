#pragma once

#include "esp_err.h"

/*
 * Starts the on-chip Ethernet MAC with the board's PHY and requests an
 * address over DHCP. Link and address changes are logged. The interface has
 * a higher route priority than the camera Wi-Fi, so it stays the default
 * route while the camera network is joined; the camera remains reachable
 * through its own subnet.
 *
 * Requires esp_netif_init() and the default event loop.
 */
esp_err_t ethernet_uplink_start(void);
