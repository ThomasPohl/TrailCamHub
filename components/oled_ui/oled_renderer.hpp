#pragma once

// Draws a status screen into the frame of a 128x64 monochrome OLED (SSD1306)
// in its page format: 8 pages of 128 bytes, bit 0 of a byte is the top pixel
// of its page. No ESP-IDF dependencies, so it is unit-tested on the host
// (#224). The text is the dongle display's (status_screen.hpp), cut to what
// fits.

#include <cstddef>
#include <cstdint>

#include "status_screen.hpp"

namespace ui
{

constexpr int kOledWidth = 128;
constexpr int kOledHeight = 64;
constexpr std::size_t kOledFrameSize = kOledWidth * kOledHeight / 8;
// Characters of a line at the font's single size (6 pixels per character).
constexpr std::size_t kOledColumns = kOledWidth / 6;

// Fills `frame` (kOledFrameSize bytes). The title is drawn at double size when
// it fits the width, otherwise at single size; long lines are cut.
void render_oled(const Screen &screen, std::uint8_t *frame);

}  // namespace ui
