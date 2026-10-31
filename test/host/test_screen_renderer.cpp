#include <doctest/doctest.h>

#include <vector>

#include "screen_renderer.hpp"

using namespace ui;

TEST_CASE("render draws title, text and hint on black")
{
    std::vector<std::uint16_t> frame(kScreenWidth * kScreenHeight, 0x1234);
    const Screen screen = make_screen(Phase::connected, "CAM", 0);
    render(screen, frame.data());

    std::size_t title = 0;
    std::size_t text = 0;
    std::size_t hint = 0;
    std::size_t background = 0;
    for (std::uint16_t pixel : frame) {
        if (pixel == rgb565(0, 220, 80))
            title++;
        else if (pixel == rgb565(255, 255, 255))
            text++;
        else if (pixel == rgb565(140, 140, 140))
            hint++;
        else if (pixel == 0)
            background++;
    }
    CHECK(title > 0);
    CHECK(text > 0);
    CHECK(hint > 0);
    // Only these colours: nothing of the old content is left.
    CHECK(title + text + hint + background == frame.size());
}
