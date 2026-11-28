#pragma once
#include "../../esp_stubs/esp_log.h"
// Host: log levels per tag are accepted and ignored (#446).
extern "C" esp_log_level_t esp_log_level_get(const char *tag);
extern "C" void esp_log_level_set(const char *tag, esp_log_level_t level);
