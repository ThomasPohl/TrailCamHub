#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ip4)                                                            \
    (int)((ip4)->addr & 0xff), (int)(((ip4)->addr >> 8) & 0xff),               \
        (int)(((ip4)->addr >> 16) & 0xff), (int)(((ip4)->addr >> 24) & 0xff)
