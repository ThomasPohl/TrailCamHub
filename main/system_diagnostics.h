#pragma once

#include "esp_err.h"

/*
 * Logs the reset reason, the detected PSRAM size and whether a core dump is
 * stored in flash, then starts periodic memory logging when
 * CONFIG_DIAG_MEMORY_LOG_INTERVAL_S is non-zero.
 */
esp_err_t diagnostics_start(void);

/*
 * Logs free and minimum-ever free heap for internal RAM and PSRAM. `stage`
 * names the point in the workflow, for example "BLE started".
 */
void diagnostics_log_memory(const char *stage);
