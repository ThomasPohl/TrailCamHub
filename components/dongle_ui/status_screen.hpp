#pragma once

// Text content of the status screen. It has no ESP-IDF dependencies so it can
// be unit-tested on the host.

#include <cstddef>
#include <cstdint>

namespace ui
{

enum class Phase : std::uint8_t {
    idle,
    searching,
    connecting,
    connected,
    retry_wait,
    // Idle after a session that did all its work.
    finished,
};

enum class Tone : std::uint8_t {
    neutral,
    busy,
    ok,
    warning,
};

// Characters per line at the font scales used by the renderer.
constexpr std::size_t kTitleColumns = 13;
constexpr std::size_t kLineColumns = 26;

struct Screen {
    char title[kTitleColumns + 1];
    char line1[kLineColumns + 1];
    char line2[kLineColumns + 1];
    char line3[kLineColumns + 1];
    char hint[kLineColumns + 1];
    Tone tone;
};

// Media download progress of the running session; total 0 when nothing is
// being downloaded.
struct MediaProgress {
    std::uint32_t done;    // files handled
    std::uint32_t stored;  // of them saved to the SD card
    std::uint32_t total;
};

// How to reach the web UI; shown while idle. Null members: no web UI.
struct WebAccess {
    const char *ssid;
    const char *key;
    const char *url;
};

// `ssid` may be null or empty. Characters outside printable ASCII are
// replaced by '?'. `media` is only shown while connected.
Screen make_screen(Phase phase, const char *ssid, std::uint32_t retry_in_s,
                   MediaProgress media = {0, 0, 0},
                   WebAccess web = {nullptr, nullptr, nullptr});

bool same_content(const Screen &a, const Screen &b);

}  // namespace ui
