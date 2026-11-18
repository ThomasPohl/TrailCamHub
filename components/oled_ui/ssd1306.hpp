#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace ssd1306
{

// Adds the display at `address` to the I2C bus `bus` (an
// i2c_master_bus_handle_t) and initialises it (128x64, charge pump on).
esp_err_t init(void *bus, std::uint8_t address);

// Sends a frame in page format (8 pages of 128 bytes).
esp_err_t present(const std::uint8_t *frame, std::size_t size);

}  // namespace ssd1306
