#pragma once

#include <stdint.h>

#include "dongle_ui.h"
#include "esp_err.h"

/*
 * Status display on a 128x64 SSD1306 OLED over I2C for boards without a
 * display (#224). It shows the same screens as the T-Dongle's display, with
 * the application reporting the phase as for the dongle (dongle_ui_phase_t).
 */

esp_err_t oled_ui_start(void);
void oled_ui_set_status(dongle_ui_phase_t phase, const char *ssid,
                        uint32_t retry_delay_ms);
void oled_ui_set_media_progress(uint32_t done, uint32_t stored, uint32_t total);
