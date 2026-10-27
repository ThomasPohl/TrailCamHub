#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway);
