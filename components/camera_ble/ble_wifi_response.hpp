#pragma once

// Reassembly of the camera's framed Wi-Fi response from BLE notifications.
// A frame is: 13 57 <message type:2 LE> <JSON length:4 LE> <JSON>.
// This module has no ESP-IDF dependencies so it can be unit-tested on the
// host.

#include <array>
#include <cstddef>
#include <cstdint>

namespace camera::ble
{

class WifiResponseAssembler
{
  public:
    static constexpr std::size_t kHeaderSize = 8;
    static constexpr std::size_t kMaxJsonSize = 512;
    static constexpr std::uint8_t kMagic0 = 0x13;
    static constexpr std::uint8_t kMagic1 = 0x57;

    enum class Event {
        none,
        // The header is complete; json_size() is the announced JSON length.
        header_received,
        // A complete frame is available until the next push() or reset().
        frame_complete,
        // The announced JSON length is zero or too large; frame discarded.
        invalid_length,
        // The frame did not fit into the buffer; frame discarded.
        overflow,
    };

    // Adds one received byte and reports what happened.
    Event push(std::uint8_t byte);
    void reset();

    std::uint16_t message_type() const;
    std::uint32_t json_size() const;
    // JSON of the completed frame (not NUL-terminated).
    const std::uint8_t *json() const;

  private:
    std::array<std::uint8_t, kHeaderSize + kMaxJsonSize> frame_{};
    std::size_t size_{};
    std::size_t expected_size_{};
    std::uint32_t json_size_{};
};

}  // namespace camera::ble
