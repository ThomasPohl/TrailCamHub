#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace camera
{

class UdpRequest
{
  public:
    virtual ~UdpRequest() = default;

    virtual const std::uint8_t *data() const = 0;
    virtual std::size_t size() const = 0;
};

// Packet types in the second byte of camera UDP packets (first byte 0xF1).
// The framing follows the PPPP/CS2 P2P protocol.
enum class PacketType : std::uint8_t {
    discovery_request = 0x41,
    discovery_reply = 0x42,
    data = 0xd0,
    data_ack = 0xd1,
    alive = 0xe0,
    alive_ack = 0xe1,
    close = 0xf0,
    pre_login = 0xf9,
};

// Returns true and sets `type` if `bytes` is a 4-byte control packet
// (F1 <type> 00 00) without payload, such as ALIVE or CLOSE.
bool parse_control_packet(const std::uint8_t *bytes, std::size_t size,
                          PacketType *type);

// Control packet without payload: F1 <type> 00 00.
class ControlRequest final : public UdpRequest
{
  public:
    explicit ControlRequest(PacketType type);

    const std::uint8_t *data() const override;
    std::size_t size() const override;

  private:
    std::array<std::uint8_t, 4> bytes_{};
};

class CameraDiscoveryRequest final : public UdpRequest
{
  public:
    static constexpr std::size_t kSize = 24;

    const std::uint8_t *data() const override;
    std::size_t size() const override;

  private:
    // clang-format off
    static constexpr std::array<std::uint8_t, kSize> kBytes = {
        0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
    };
    // clang-format on
};

class PreLoginRequest final : public UdpRequest
{
  public:
    static constexpr std::size_t kMaxPacketSize = 128;

    bool build(const std::uint8_t *body, std::size_t body_size);
    const std::uint8_t *data() const override;
    std::size_t size() const override;

  private:
    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisCommandRequest final : public UdpRequest
{
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

// Reliable-UDP packet (PPPP "DRW"): F1 <D0|D1> <length:2> D1 <channel>
// <word:2> <payload>. For DATA the 16-bit word is the packet index; for an
// ACK it is the number of acknowledged indices, which follow as payload.
class ArtemisRudpPacket : public UdpRequest
{
  public:
    static constexpr std::size_t kHeaderSize = 8;
    static constexpr std::size_t kMaxPacketSize = 512;
    static constexpr std::uint8_t kDataType = 0xd0;
    static constexpr std::uint8_t kAckType = 0xd1;
    static constexpr std::uint8_t kChannel = 0;

    bool build(std::uint8_t type, std::uint16_t header_word,
               const std::uint8_t *payload, std::size_t payload_size);
    bool parse(const std::uint8_t *bytes, std::size_t size);

    bool is_data() const;
    bool is_ack() const;
    // Index of a DATA packet.
    std::uint16_t index() const;
    // Number of indices acknowledged by an ACK packet.
    std::uint16_t ack_count() const;
    bool acknowledges(std::uint16_t index) const;
    const std::uint8_t *data() const override;
    std::size_t size() const override;

  private:
    std::uint16_t header_word() const;

    std::array<std::uint8_t, kMaxPacketSize> bytes_{};
    std::size_t size_{};
};

class ArtemisDataRequest final : public ArtemisRudpPacket
{
  public:
    bool build(std::uint16_t index, const std::uint8_t *payload,
               std::size_t payload_size);
};

// Acknowledges one DATA packet, in the same format the camera uses:
// F1 D1 00 06 D1 00 00 01 <index:2>.
class ArtemisAckRequest final : public ArtemisRudpPacket
{
  public:
    bool build(std::uint16_t received_index);
};

// Handshake packets from the reference implementation, sent with exactly
// the bytes used so far. In the DRW format they read as ACKs of index 0
// with inconsistent counts; their purpose is not yet understood.
class ArtemisMagicRequest final : public ArtemisRudpPacket
{
  public:
    enum class Stage {
        first,
        second,
    };

    bool build(Stage stage);
};

class ArtemisHeartbeatPayload
{
  public:
    static constexpr std::size_t kSize = 45;

    ArtemisHeartbeatPayload();
    const std::uint8_t *next_command();
    std::size_t size() const;
    std::uint8_t command_count() const;

  private:
    // clang-format off
    static constexpr std::array<std::uint8_t, 12> kPrefix = {
        0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00,
        0x02, 0x00, 0x00, 0x00,
    };
    // clang-format on
    // clang-format off
    static constexpr std::array<std::uint8_t, 32> kSuffix = {
        0x00, 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x4d,
        0x7a, 0x6c, 0x42, 0x33, 0x36, 0x58, 0x2f, 0x49,
        0x56, 0x6f, 0x38, 0x5a, 0x7a, 0x49, 0x35, 0x72,
        0x47, 0x39, 0x6a, 0x31, 0x77, 0x3d, 0x3d, 0x00,
    };
    // clang-format on

    std::array<std::uint8_t, kSize> bytes_{};
    std::uint8_t command_count_{1};
};

}  // namespace camera
