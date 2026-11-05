#include "status_screen.hpp"

#include <cstdio>
#include <cstring>

namespace ui
{

namespace
{

constexpr std::uint32_t kMaxCount = 9999;

template <std::size_t N> void set(char (&target)[N], const char *text)
{
    std::snprintf(target, N, "%s", text);
}

void set_ssid(char *target, std::size_t size, const char *ssid,
              const char *fallback)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        std::snprintf(target, size, "%s", fallback);
        return;
    }
    std::size_t length = 0;
    for (; ssid[length] != '\0' && length + 1 < size; length++) {
        const unsigned char c = static_cast<unsigned char>(ssid[length]);
        target[length] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    target[length] = '\0';
}

// Network name, password and address of the web UI on the lower lines.
void set_web_access(Screen &screen, const WebAccess &web)
{
    char text[96];
    std::snprintf(text, sizeof(text), "WiFi %s", web.ssid);
    set_ssid(screen.line2, sizeof(screen.line2), text, "");
    std::snprintf(text, sizeof(text), "Key %s",
                  web.key != nullptr ? web.key : "");
    set_ssid(screen.line3, sizeof(screen.line3), text, "");
    set(screen.hint, web.url != nullptr ? web.url : "");
}

// The lower lines of an idle screen: the web UI access, or how to search.
void set_idle_lines(Screen &screen, const WebAccess &web)
{
    if (web.ssid == nullptr || web.ssid[0] == '\0') {
        set(screen.line2, "Press button to");
        set(screen.line3, "search again");
        return;
    }
    set_web_access(screen, web);
}

// "Saved 2 of 3 files", or "Camera synced" without downloads.
void set_media_result(char (&line)[kLineColumns + 1],
                      const MediaProgress &media)
{
    if (media.total == 0) {
        set(line, "Camera synced");
        return;
    }
    const unsigned long stored =
        media.stored < kMaxCount ? media.stored : kMaxCount;
    const unsigned long total =
        media.total < kMaxCount ? media.total : kMaxCount;
    // Counts are clamped, so the text fits; the buffer avoids a truncation
    // warning.
    char text[48];
    std::snprintf(text, sizeof(text), "Saved %lu of %lu files", stored, total);
    set(line, text);
}

// "Saved 2 of 3 files" of a session that ended early, or "Session ended
// early" when it ended before the downloads.
void set_partial_result(char (&line)[kLineColumns + 1],
                        const MediaProgress &media)
{
    if (media.total == 0) {
        set(line, "Session ended early");
        return;
    }
    set_media_result(line, media);
}

}  // namespace

Screen make_screen(Phase phase, const char *ssid, std::uint32_t retry_in_s,
                   MediaProgress media, WebAccess web)
{
    Screen screen = {};
    switch (phase) {
    case Phase::idle:
        screen.tone = Tone::neutral;
        set(screen.title, "READY");
        if (web.ssid == nullptr || web.ssid[0] == '\0') {
            set(screen.line1, "Press button to");
            set(screen.line2, "search for camera");
            break;
        }
        set(screen.line1, "Press button to search");
        set_web_access(screen, web);
        break;
    case Phase::searching:
        screen.tone = Tone::busy;
        set(screen.title, "SEARCHING");
        set(screen.line1, "Looking for camera");
        set(screen.line2, "via Bluetooth");
        set(screen.hint, "Hold button: stop");
        break;
    case Phase::connecting:
        screen.tone = Tone::busy;
        set(screen.title, "CONNECTING");
        set_ssid(screen.line1, sizeof(screen.line1), ssid, "Camera Wi-Fi");
        set(screen.line2, "Joining network");
        set(screen.hint, "Hold button: stop");
        break;
    case Phase::connected: {
        screen.tone = Tone::ok;
        set(screen.title, "CONNECTED");
        set_ssid(screen.line1, sizeof(screen.line1), ssid, "Camera Wi-Fi");
        // Counts are clamped so the line always fits.
        char progress[48];
        const unsigned long total =
            media.total < kMaxCount ? media.total : kMaxCount;
        if (media.total == 0) {
            std::snprintf(progress, sizeof(progress), "Session running");
        } else if (media.done < media.total) {
            const unsigned long current =
                media.done < kMaxCount ? media.done + 1 : kMaxCount;
            std::snprintf(progress, sizeof(progress), "Saving media %lu/%lu",
                          current, total);
        } else {
            std::snprintf(progress, sizeof(progress), "Media saved (%lu)",
                          total);
        }
        set(screen.line2, progress);
        set(screen.hint, "Hold button: stop");
        break;
    }
    case Phase::finished:
        screen.tone = Tone::ok;
        set(screen.title, "FINISHED");
        set_media_result(screen.line1, media);
        set_idle_lines(screen, web);
        break;
    case Phase::partial:
        screen.tone = Tone::warning;
        set(screen.title, "INCOMPLETE");
        set_partial_result(screen.line1, media);
        set_idle_lines(screen, web);
        break;
    case Phase::not_found:
        screen.tone = Tone::warning;
        set(screen.title, "NOT FOUND");
        set(screen.line1, "Camera not reached");
        set_idle_lines(screen, web);
        break;
    case Phase::no_card:
        screen.tone = Tone::warning;
        set(screen.title, "NO SD CARD");
        set(screen.line1, "Camera has no SD card");
        set_idle_lines(screen, web);
        break;
    case Phase::upload_failed:
        screen.tone = Tone::warning;
        set(screen.title, "NO UPLOAD");
        // Also after an upload-only cycle, which has no media counts.
        set(screen.line1, "Home Wi-Fi not reached");
        set_idle_lines(screen, web);
        break;
    case Phase::stopped:
        screen.tone = Tone::neutral;
        set(screen.title, "STOPPED");
        set(screen.line1, "Search stopped");
        set_idle_lines(screen, web);
        break;
    case Phase::retry_wait:
        screen.tone = Tone::warning;
        set(screen.title, "NO LINK");
        std::snprintf(screen.line1, sizeof(screen.line1), "Next try in %lu s",
                      static_cast<unsigned long>(retry_in_s));
        set(screen.hint, "Press: now  Hold: stop");
        break;
    }
    return screen;
}

bool same_content(const Screen &a, const Screen &b)
{
    return a.tone == b.tone && std::strcmp(a.title, b.title) == 0 &&
           std::strcmp(a.line1, b.line1) == 0 &&
           std::strcmp(a.line2, b.line2) == 0 &&
           std::strcmp(a.line3, b.line3) == 0 &&
           std::strcmp(a.hint, b.hint) == 0;
}

}  // namespace ui
