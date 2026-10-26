#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace camera {

class CameraDiscoveryPacket {
public:
    static constexpr std::size_t kSize = 24;

    const std::uint8_t *data() const;
    std::size_t size() const;

private:
    static constexpr std::array<std::uint8_t, kSize> kBytes = {
        0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
    };
};

class ArtemisRudpPacket {
public:
    static constexpr std::size_t kHeaderSize = 8;
    static constexpr std::size_t kMaxPacketSize = 512;
    static constexpr std::uint8_t kDataType = 0xd0;
    static constexpr std::uint8_t kAckType = 0xd1;

    bool build(std::uint8_t type, std::uint8_t sequence,
               const std::uint8_t *payload, std::size_t payload_size);
    bool parse(const std::uint8_t *bytes, std::size_t size);

    bool is_data() const;
    bool is_ack() const;
    bool acknowledges(std::uint8_t sequence) const;
    std::uint8_t sequence() const;
    const std::uint8_t *data() const;
    std::size_t size() const;

private:
    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisHeartbeatPayload {
public:
    static constexpr std::size_t kSize = 45;

    ArtemisHeartbeatPayload();
    const std::uint8_t *next_command();
    std::size_t size() const;
    std::uint8_t command_count() const;

private:
    static constexpr std::array<std::uint8_t, 12> kPrefix = {
        0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00,
        0x02, 0x00, 0x00, 0x00,
    };
    static constexpr std::array<std::uint8_t, 32> kSuffix = {
        0x00, 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x4d,
        0x7a, 0x6c, 0x42, 0x33, 0x36, 0x58, 0x2f, 0x49,
        0x56, 0x6f, 0x38, 0x5a, 0x7a, 0x49, 0x35, 0x72,
        0x47, 0x39, 0x6a, 0x31, 0x77, 0x3d, 0x3d, 0x00,
    };

    std::array<std::uint8_t, kSize> bytes_{};
    std::uint8_t command_count_{1};
};

}  // namespace camera
