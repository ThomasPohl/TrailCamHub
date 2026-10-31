#pragma once

#include <stdint.h>

#include "esp_err.h"

// The APA102 RGB LED of the T-Dongle. Without data it shows a random colour
// at full brightness, so it is driven even when it is meant to be off.

// Configures the clock and data pins and switches the LED off.
esp_err_t status_led_init(void);

// Sets the colour; `brightness` is the APA102 global brightness (0-31, 0 is
// off).
void status_led_set(uint8_t red, uint8_t green, uint8_t blue,
                    uint8_t brightness);
