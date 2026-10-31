#include "button_detector.hpp"

namespace ui
{

ButtonDetector::ButtonDetector(std::uint32_t debounce_ms,
                               std::uint32_t long_press_ms)
    : debounce_ms_(debounce_ms), long_press_ms_(long_press_ms)
{
}

ButtonDetector::Event ButtonDetector::update(bool pressed, std::uint32_t now_ms)
{
    if (pressed != raw_) {
        raw_ = pressed;
        raw_changed_ms_ = now_ms;
    }
    if (raw_ != stable_ && now_ms - raw_changed_ms_ >= debounce_ms_) {
        stable_ = raw_;
        if (stable_) {
            pressed_ms_ = now_ms;
            long_reported_ = false;
        } else if (!long_reported_) {
            return Event::short_press;
        }
        return Event::none;
    }
    if (stable_ && !long_reported_ && now_ms - pressed_ms_ >= long_press_ms_) {
        long_reported_ = true;
        return Event::long_press;
    }
    return Event::none;
}

}  // namespace ui
