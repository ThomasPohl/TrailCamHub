#pragma once
#include <stddef.h>
#include <stdint.h>
extern "C" uint32_t esp_random(void);
extern "C" void esp_fill_random(void *buf, size_t len);
