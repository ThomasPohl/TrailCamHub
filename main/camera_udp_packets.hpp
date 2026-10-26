#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace camera {

class UdpRequest {
public:
    virtual ~UdpRequest() = default;

    virtual const std::uint8_t *data() const = 0;
    virtual std::size_t size() const = 0;
};

class CameraDiscoveryRequest final : public UdpRequest {
public:
    static constexpr std::size_t kSize = 24;

    const std::uint8_t *data() const override;
    std::size_t size() const override;

private:
    static constexpr std::array<std::uint8_t, kSize> kBytes = {
        0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
    };
};

class PreLoginRequest final : public UdpRequest {
public:
    static constexpr std::size_t kMaxPacketSize = 128;

    bool build(const std::uint8_t *body, std::size_t body_size);
    const std::uint8_t *data() const override;
    std::size_t size() const override;

private:
    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisCommandRequest final : public UdpRequest {
public:
    static constexpr std::size_t kHeaderSize = 20;
    static constexpr std::size_t kMaxPacketSize = 512;
    static constexpr std::uint32_t kCommandRequestType = 2;

    bool build(std::uint32_t app_sequence, const char *base64_payload,
               std::size_t payload_size);
    const std::uint8_t *data() const override;
    std::size_t size() const override;

private:
    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisRudpPacket : public UdpRequest {
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
    const std::uint8_t *data() const override;
    std::size_t size() const override;

private:
    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisDataRequest final : public ArtemisRudpPacket {
public:
    bool build(std::uint8_t sequence, const std::uint8_t *payload,
               std::size_t payload_size);
};

class ArtemisAckRequest final : public ArtemisRudpPacket {
public:
    static constexpr std::size_t kPayloadSize = 4;

    bool build(std::uint8_t local_sequence, std::uint8_t received_sequence);
};

class ArtemisMagicRequest final : public ArtemisRudpPacket {
public:
    enum class Stage {
        first,
        second,
    };

    bool build(std::uint8_t sequence, Stage stage);
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
