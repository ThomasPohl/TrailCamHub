#pragma once
#include "../../esp_stubs/esp_attr.h"
// Host: RTC memory is ordinary memory; it survives a simulated deep sleep
// because the process keeps running (#446).
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
