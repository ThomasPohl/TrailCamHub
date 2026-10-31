#include <doctest/doctest.h>

#include "button_detector.hpp"

using ui::ButtonDetector;

namespace
{

constexpr std::uint32_t kDebounceMs = 30;
constexpr std::uint32_t kLongPressMs = 1500;

// Feeds a constant level in 10 ms steps from `from_ms` up to (excluding)
// `to_ms` and returns the first event other than none.
ButtonDetector::Event hold(ButtonDetector &button, bool pressed,
                           std::uint32_t from_ms, std::uint32_t to_ms)
{
    ButtonDetector::Event first = ButtonDetector::Event::none;
    for (std::uint32_t now = from_ms; now < to_ms; now += 10) {
        const ButtonDetector::Event event = button.update(pressed, now);
        if (first == ButtonDetector::Event::none) {
            first = event;
        }
    }
    return first;
}

}  // namespace

TEST_CASE("short press is reported on release")
{
    ButtonDetector button(kDebounceMs, kLongPressMs);
    CHECK(hold(button, false, 0, 100) == ButtonDetector::Event::none);
    CHECK(hold(button, true, 100, 400) == ButtonDetector::Event::none);
    CHECK(hold(button, false, 400, 500) == ButtonDetector::Event::short_press);
}

TEST_CASE("long press is reported once while held, without a short press")
{
    ButtonDetector button(kDebounceMs, kLongPressMs);
    CHECK(hold(button, true, 0, 1600) == ButtonDetector::Event::long_press);
    CHECK(hold(button, true, 1600, 5000) == ButtonDetector::Event::none);
    CHECK(hold(button, false, 5000, 5100) == ButtonDetector::Event::none);
}

TEST_CASE("bounces shorter than the debounce time are ignored")
{
    ButtonDetector button(kDebounceMs, kLongPressMs);
    for (std::uint32_t now = 0; now < 200; now += 20) {
        CHECK(button.update((now / 20) % 2 == 0, now) ==
              ButtonDetector::Event::none);
    }
    CHECK(hold(button, false, 200, 300) == ButtonDetector::Event::none);
}

TEST_CASE("timestamps may wrap around")
{
    ButtonDetector button(kDebounceMs, kLongPressMs);
    const std::uint32_t start = 0xffffffffu - 50;
    std::uint32_t now = start;
    ButtonDetector::Event event = ButtonDetector::Event::none;
    for (int i = 0; i < 20; i++, now += 10) {
        event = button.update(true, now);
        CHECK(event == ButtonDetector::Event::none);
    }
    for (int i = 0; i < 10 && event == ButtonDetector::Event::none;
         i++, now += 10) {
        event = button.update(false, now);
    }
    CHECK(event == ButtonDetector::Event::short_press);
}
