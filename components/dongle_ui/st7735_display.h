#pragma once

#include <stdint.h>

#include "esp_err.h"

// Landscape orientation of the 0.96" ST7735 panel of the T-Dongle-S3.
#define ST7735_WIDTH 160
#define ST7735_HEIGHT 80

// Initializes SPI, the panel and the backlight.
esp_err_t st7735_init(void);

// Sends a full frame of ST7735_WIDTH * ST7735_HEIGHT big-endian RGB565 pixels
// and returns when the transfer has finished. `frame` must be DMA-capable.
esp_err_t st7735_present(const uint16_t *frame);
