#pragma once

#include <cstdint>

namespace ui
{

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;

// Returns the five columns of the glyph for a printable ASCII character
// (bit 0 is the top row); other characters give the glyph for '?'.
const std::uint8_t *glyph(char c);

}  // namespace ui
