#include "ble_wifi_response.hpp"

namespace camera::ble
{

void WifiResponseAssembler::reset()
{
    size_ = 0;
    expected_size_ = 0;
    json_size_ = 0;
}

WifiResponseAssembler::Event WifiResponseAssembler::push(std::uint8_t byte)
{
    if (expected_size_ != 0 && size_ == expected_size_) {
        // The previous frame was completed; start a new one.
        reset();
    }
    if (size_ == 0) {
        if (byte == kMagic0) {
            frame_[size_++] = byte;
        }
        return Event::none;
    }
    if (size_ == 1 && byte != kMagic1) {
        reset();
        if (byte == kMagic0) {
            frame_[size_++] = byte;
        }
        return Event::none;
    }
    if (size_ >= frame_.size()) {
        reset();
        return Event::overflow;
    }
    frame_[size_++] = byte;

    if (size_ == kHeaderSize) {
        const std::uint32_t json_size =
            static_cast<std::uint32_t>(frame_[4]) |
            (static_cast<std::uint32_t>(frame_[5]) << 8) |
            (static_cast<std::uint32_t>(frame_[6]) << 16) |
            (static_cast<std::uint32_t>(frame_[7]) << 24);
        if (json_size == 0 || json_size > kMaxJsonSize) {
            json_size_ = json_size;
            size_ = 0;
            expected_size_ = 0;
            return Event::invalid_length;
        }
        json_size_ = json_size;
        expected_size_ = kHeaderSize + json_size;
        return Event::header_received;
    }
    if (expected_size_ != 0 && size_ == expected_size_) {
        return Event::frame_complete;
    }
    return Event::none;
}

std::uint16_t WifiResponseAssembler::message_type() const
{
    return static_cast<std::uint16_t>(frame_[2] | (frame_[3] << 8));
}

std::uint32_t WifiResponseAssembler::json_size() const
{
    return json_size_;
}

const std::uint8_t *WifiResponseAssembler::json() const
{
    return frame_.data() + kHeaderSize;
}

}  // namespace camera::ble
