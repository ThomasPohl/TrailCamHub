#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "oled_renderer.hpp"

using namespace ui;

namespace
{

bool pixel(const std::vector<std::uint8_t> &frame, int x, int y)
{
    return (frame[(y / 8) * kOledWidth + x] & (1u << (y % 8))) != 0;
}

// Pixels set in the rows [y0, y1).
std::size_t count(const std::vector<std::uint8_t> &frame, int y0, int y1)
{
    std::size_t n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < kOledWidth; x++) {
            n += pixel(frame, x, y) ? 1 : 0;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("oled: the frame is 8 pages of 128 bytes")
{
    CHECK(kOledFrameSize == 1024);
    CHECK(kOledColumns == 21);
}

TEST_CASE("oled: an empty screen is dark, a status screen has text")
{
    std::vector<std::uint8_t> frame(kOledFrameSize, 0xff);
    Screen empty = {};
    render_oled(empty, frame.data());
    CHECK(count(frame, 0, kOledHeight) == 0);

    const Screen screen = make_screen(Phase::searching, "CAM", 0);
    render_oled(screen, frame.data());
    CHECK(count(frame, 0, 16) > 0);   // title
    CHECK(count(frame, 20, 28) > 0);  // first line
    CHECK(count(frame, 56, 63) > 0);  // hint
}

TEST_CASE("oled: nothing is drawn outside the display or between the lines")
{
    const Screen screen = make_screen(Phase::connected, "TrailCam_ABCDEF", 0);
    std::vector<std::uint8_t> frame(kOledFrameSize, 0);
    render_oled(screen, frame.data());
    // The last row below the hint stays dark and the gap between the title
    // (16 rows at double size) and the first line is empty.
    CHECK(count(frame, 63, 64) == 0);
    CHECK(count(frame, 16, 20) == 0);
}

TEST_CASE("oled: a long line is cut at the width")
{
    Screen screen = {};
    std::memset(screen.line1, 'W', kLineColumns);
    std::vector<std::uint8_t> frame(kOledFrameSize, 0);
    render_oled(screen, frame.data());
    // 20 characters of 6 pixels and a margin of 1: nothing from x = 121 on.
    for (int x = 121; x < kOledWidth; x++) {
        for (int y = 0; y < kOledHeight; y++) {
            CHECK_FALSE(pixel(frame, x, y));
        }
    }
    CHECK(count(frame, 20, 28) > 0);
}

TEST_CASE("oled: the title is double size when it fits, single otherwise")
{
    Screen shortTitle = {};
    std::strcpy(shortTitle.title, "SEARCHING");  // 9 characters: 108 pixels
    std::vector<std::uint8_t> frame(kOledFrameSize, 0);
    render_oled(shortTitle, frame.data());
    CHECK(count(frame, 8, 14) > 0);  // rows below a single-size title

    Screen longTitle = {};
    std::strcpy(longTitle.title, "NOT FOUND ABCD");  // 14: too wide at scale 2
    std::fill(frame.begin(), frame.end(), 0);
    render_oled(longTitle, frame.data());
    CHECK(count(frame, 8, 16) == 0);  // single size ends at row 7
    CHECK(count(frame, 0, 8) > 0);
}
