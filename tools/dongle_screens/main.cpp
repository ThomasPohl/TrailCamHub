// Renders the status screens of the T-Dongle as PNG files (see
// CMakeLists.txt). The network name and password are example values.

#include <cstdio>
#include <string>
#include <vector>

#include "png_writer.hpp"
#include "screen_renderer.hpp"
#include "status_screen.hpp"

namespace
{

constexpr int kScale = 4;
constexpr int kBezel = 6;

const ui::WebAccess kWeb = {"TrailCamHub-F4A1", "hx4mk7rta2",
                            "http://10.42.0.1"};

struct Example {
    const char *name;
    ui::Screen screen;
};

// Scales the frame up and adds a dark bezel like the panel's frame.
std::vector<std::uint8_t> to_rgb(const std::vector<std::uint16_t> &frame,
                                 int &width, int &height)
{
    width = ui::kScreenWidth * kScale + 2 * kBezel;
    height = ui::kScreenHeight * kScale + 2 * kBezel;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3,
                                  0x1c);
    for (int y = 0; y < ui::kScreenHeight * kScale; y++) {
        for (int x = 0; x < ui::kScreenWidth * kScale; x++) {
            const std::uint16_t pixel =
                frame[(y / kScale) * ui::kScreenWidth + x / kScale];
            // RGB565 to 8 bits per channel, as the panel shows it.
            const std::uint8_t r = ((pixel >> 11) & 0x1f) * 255 / 31;
            const std::uint8_t g = ((pixel >> 5) & 0x3f) * 255 / 63;
            const std::uint8_t b = (pixel & 0x1f) * 255 / 31;
            const std::size_t at =
                (static_cast<std::size_t>(y + kBezel) * width + x + kBezel) * 3;
            rgb[at] = r;
            rgb[at + 1] = g;
            rgb[at + 2] = b;
        }
    }
    return rgb;
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string directory = argc > 1 ? argv[1] : ".";
    const Example examples[] = {
        {"ready", ui::make_screen(ui::Phase::idle, "", 0, {0, 0, 0}, kWeb)},
        {"searching", ui::make_screen(ui::Phase::searching, "", 0)},
        {"connecting",
         ui::make_screen(ui::Phase::connecting, "TrailCam_DA63", 0)},
        {"connected",
         ui::make_screen(ui::Phase::connected, "TrailCam_DA63", 0, {1, 1, 3})},
        {"retry", ui::make_screen(ui::Phase::retry_wait, "", 12)},
        {"finished", ui::make_screen(ui::Phase::finished, "TrailCam_DA63", 0,
                                     {3, 3, 3}, kWeb)},
    };
    std::vector<std::uint16_t> frame(ui::kScreenWidth * ui::kScreenHeight);
    for (const Example &example : examples) {
        ui::render(example.screen, frame.data());
        int width = 0;
        int height = 0;
        const std::vector<std::uint8_t> rgb = to_rgb(frame, width, height);
        const std::string path = directory + "/" + example.name + ".png";
        if (!write_png(path, width, height, rgb)) {
            std::fprintf(stderr, "Could not write %s\n", path.c_str());
            return 1;
        }
        std::printf("%s\n", path.c_str());
    }
    return 0;
}
