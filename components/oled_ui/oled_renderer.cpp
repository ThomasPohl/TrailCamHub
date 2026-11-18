#include "oled_renderer.hpp"

#include <cstring>

#include "font5x7.hpp"

namespace ui
{

namespace
{

constexpr int kMarginX = 1;
constexpr int kTitleY = 0;
constexpr int kLine1Y = 20;
constexpr int kLine2Y = 31;
constexpr int kLine3Y = 42;
constexpr int kHintY = 56;

void set_pixel(std::uint8_t *frame, int x, int y)
{
    if (x < 0 || x >= kOledWidth || y < 0 || y >= kOledHeight) {
        return;
    }
    frame[(y / 8) * kOledWidth + x] |= static_cast<std::uint8_t>(1u << (y % 8));
}

void draw_char(std::uint8_t *frame, int x, int y, int scale, char c)
{
    const std::uint8_t *columns = glyph(c);
    for (int column = 0; column < kGlyphWidth; column++) {
        for (int row = 0; row < kGlyphHeight; row++) {
            if ((columns[column] & (1u << row)) == 0) {
                continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    set_pixel(frame, x + column * scale + dx,
                              y + row * scale + dy);
                }
            }
        }
    }
}

// Draws at most `max_chars` characters.
void draw_text(std::uint8_t *frame, int x, int y, int scale, const char *text,
               std::size_t max_chars)
{
    const int advance = (kGlyphWidth + 1) * scale;
    for (std::size_t i = 0; text[i] != '\0' && i < max_chars;
         i++, x += advance) {
        draw_char(frame, x, y, scale, text[i]);
    }
}

}  // namespace

void render_oled(const Screen &screen, std::uint8_t *frame)
{
    std::memset(frame, 0, kOledFrameSize);
    const std::size_t title_len = std::strlen(screen.title);
    const int title_scale =
        static_cast<int>(title_len) * (kGlyphWidth + 1) * 2 + kMarginX <=
                kOledWidth
            ? 2
            : 1;
    draw_text(frame, kMarginX, kTitleY, title_scale, screen.title,
              kOledColumns);
    draw_text(frame, kMarginX, kLine1Y, 1, screen.line1, kOledColumns - 1);
    draw_text(frame, kMarginX, kLine2Y, 1, screen.line2, kOledColumns - 1);
    draw_text(frame, kMarginX, kLine3Y, 1, screen.line3, kOledColumns - 1);
    draw_text(frame, kMarginX, kHintY, 1, screen.hint, kOledColumns - 1);
}

}  // namespace ui
