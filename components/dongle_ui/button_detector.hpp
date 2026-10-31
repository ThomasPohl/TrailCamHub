#pragma once

// Debounces a push button and tells short from long presses. It has no
// ESP-IDF dependencies so it can be unit-tested on the host.

#include <cstdint>

namespace ui
{

class ButtonDetector
{
  public:
    enum class Event : std::uint8_t {
        none,
        // Released before the long-press time.
        short_press,
        // Reported once while the button is still held.
        long_press,
    };

    ButtonDetector(std::uint32_t debounce_ms, std::uint32_t long_press_ms);

    // Call periodically with the raw button level.
    Event update(bool pressed, std::uint32_t now_ms);

  private:
    std::uint32_t debounce_ms_;
    std::uint32_t long_press_ms_;
    bool raw_{false};
    std::uint32_t raw_changed_ms_{0};
    bool stable_{false};
    std::uint32_t pressed_ms_{0};
    bool long_reported_{false};
};

}  // namespace ui
