#include "screen_renderer.hpp"

#include "font5x7.hpp"

namespace ui
{

namespace
{

// The title at double size (13 columns), the other lines at single size (26
// columns).
constexpr int kMarginX = 2;
constexpr int kTitleY = 5;
constexpr int kTitleScale = 2;
constexpr int kLine1Y = 25;
constexpr int kLine2Y = 37;
constexpr int kLine3Y = 49;
constexpr int kHintY = 66;

constexpr std::uint16_t kBackground = rgb565(0, 0, 0);
constexpr std::uint16_t kText = rgb565(255, 255, 255);
constexpr std::uint16_t kHint = rgb565(140, 140, 140);

std::uint16_t tone_color(Tone tone)
{
    switch (tone) {
    case Tone::busy:
        return rgb565(255, 200, 0);
    case Tone::ok:
        return rgb565(0, 220, 80);
    case Tone::warning:
        return rgb565(255, 90, 40);
    case Tone::neutral:
        break;
    }
    return rgb565(255, 255, 255);
}

void draw_char(std::uint16_t *frame, int x, int y, int scale, char c,
               std::uint16_t color)
{
    const std::uint8_t *columns = glyph(c);
    for (int column = 0; column < kGlyphWidth; column++) {
        for (int row = 0; row < kGlyphHeight; row++) {
            if ((columns[column] & (1u << row)) == 0) {
                continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                const int py = y + row * scale + dy;
                if (py < 0 || py >= kScreenHeight) {
                    continue;
                }
                for (int dx = 0; dx < scale; dx++) {
                    const int px = x + column * scale + dx;
                    if (px >= 0 && px < kScreenWidth) {
                        frame[py * kScreenWidth + px] = color;
                    }
                }
            }
        }
    }
}

void draw_text(std::uint16_t *frame, int x, int y, int scale, const char *text,
               std::uint16_t color)
{
    const int advance = (kGlyphWidth + 1) * scale;
    for (; *text != '\0'; text++, x += advance) {
        draw_char(frame, x, y, scale, *text, color);
    }
}

}  // namespace

void render(const Screen &screen, std::uint16_t *frame)
{
    for (int i = 0; i < kScreenWidth * kScreenHeight; i++) {
        frame[i] = kBackground;
    }
    draw_text(frame, kMarginX, kTitleY, kTitleScale, screen.title,
              tone_color(screen.tone));
    draw_text(frame, kMarginX, kLine1Y, 1, screen.line1, kText);
    draw_text(frame, kMarginX, kLine2Y, 1, screen.line2, kText);
    draw_text(frame, kMarginX, kLine3Y, 1, screen.line3, kText);
    draw_text(frame, kMarginX, kHintY, 1, screen.hint, kHint);
}

}  // namespace ui
