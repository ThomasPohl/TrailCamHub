#pragma once
#include <stdint.h>
// Host: the clock of the test (see host_esp.cpp).
extern "C" int64_t esp_timer_get_time(void);
