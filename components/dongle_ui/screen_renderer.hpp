#pragma once

// Draws a status screen into a 160x80 RGB565 frame. No ESP-IDF dependencies,
// so the host tool that renders the screenshots for the documentation draws
// exactly what the dongle shows.

#include <cstdint>

#include "status_screen.hpp"

namespace ui
{

constexpr int kScreenWidth = 160;
constexpr int kScreenHeight = 80;

// RGB565 in host byte order.
constexpr std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) |
                                      (b >> 3));
}

// Fills `frame` (kScreenWidth * kScreenHeight pixels, row by row).
void render(const Screen &screen, std::uint16_t *frame);

}  // namespace ui
