#pragma once

#include "esp_err.h"

/*
 * Compares the ESP-Hosted firmware version running on the ESP32-C6
 * co-processor with the image embedded in the application. If they differ,
 * flashes the embedded image to the C6 over the ESP-Hosted link and restarts
 * the application, so the function only returns when no update was needed or
 * the update failed. Each image version is tried at most
 * CONFIG_COPROCESSOR_UPDATE_MAX_ATTEMPTS times, so a C6 that cannot be
 * updated does not cause a restart loop.
 *
 * Requires an initialized NVS.
 */
esp_err_t coprocessor_update_run(void);
