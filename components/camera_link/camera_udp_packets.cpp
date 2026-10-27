#include "camera_udp_packets.hpp"

#include <cstring>

namespace camera
{

namespace
{

void write_little_endian_u32(std::uint8_t *destination, std::uint32_t value)
{
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8);
    destination[2] = static_cast<std::uint8_t>(value >> 16);
    destination[3] = static_cast<std::uint8_t>(value >> 24);
}

}  // namespace

bool parse_control_packet(const std::uint8_t *bytes, std::size_t size,
                          PacketType *type)
{
    if (bytes == nullptr || type == nullptr || size != 4 || bytes[0] != 0xf1 ||
        bytes[2] != 0 || bytes[3] != 0) {
        return false;
    }
    *type = static_cast<PacketType>(bytes[1]);
    return true;
}

ControlRequest::ControlRequest(PacketType type)
    : bytes_{0xf1, static_cast<std::uint8_t>(type), 0x00, 0x00}
{
}

const std::uint8_t *ControlRequest::data() const
{
    return bytes_.data();
}

std::size_t ControlRequest::size() const
{
    return bytes_.size();
}

const std::uint8_t *CameraDiscoveryRequest::data() const
{
    return kBytes.data();
}

std::size_t CameraDiscoveryRequest::size() const
{
    return kBytes.size();
}

bool PreLoginRequest::build(const std::uint8_t *body, std::size_t body_size)
{
    size_ = 0;
    if (body == nullptr || body_size == 0 || body_size > bytes_.size() - 4 ||
        body_size > UINT16_MAX) {
        return false;
    }
    const std::size_t packet_size = body_size + 4;

    bytes_[0] = 0xf1;
    bytes_[1] = 0xf9;
    bytes_[2] = static_cast<std::uint8_t>(body_size >> 8);
    bytes_[3] = static_cast<std::uint8_t>(body_size);
    std::memcpy(bytes_.data() + 4, body, body_size);
    size_ = packet_size;
    return true;
}

const std::uint8_t *PreLoginRequest::data() const
{
    return bytes_.data();
}

std::size_t PreLoginRequest::size() const
{
    return size_;
}

bool ArtemisCommandRequest::build(std::uint32_t app_sequence,
                                  const char *base64_payload,
                                  std::size_t payload_size)
{
    size_ = 0;
    if (base64_payload == nullptr || payload_size == 0 ||
        payload_size > bytes_.size() - kHeaderSize - 1 ||
        payload_size > UINT32_MAX) {
        return false;
    }

    static constexpr std::uint8_t kSignature[8] = {
        'A', 'R', 'T', 'E', 'M', 'I', 'S', 0,
    };
    std::memcpy(bytes_.data(), kSignature, sizeof(kSignature));
    write_little_endian_u32(bytes_.data() + 8, kCommandRequestType);
    write_little_endian_u32(bytes_.data() + 12, app_sequence);
    write_little_endian_u32(bytes_.data() + 16,
                            static_cast<std::uint32_t>(payload_size));
    std::memcpy(bytes_.data() + kHeaderSize, base64_payload, payload_size);
    bytes_[kHeaderSize + payload_size] = 0;
    size_ = kHeaderSize + payload_size + 1;
    return true;
}

const std::uint8_t *ArtemisCommandRequest::data() const
{
    return bytes_.data();
}

std::size_t ArtemisCommandRequest::size() const
{
    return size_;
}

bool ArtemisRudpPacket::build(std::uint8_t type, std::uint16_t header_word,
                              const std::uint8_t *payload,
                              std::size_t payload_size)
{
    size_ = 0;
    const std::size_t body_size = payload_size + 4;
    if ((payload_size > 0 && payload == nullptr) ||
        payload_size > bytes_.size() - kHeaderSize || body_size > UINT16_MAX) {
        return false;
    }
    const std::size_t packet_size = payload_size + kHeaderSize;

    bytes_[0] = 0xf1;
    bytes_[1] = type;
    bytes_[2] = static_cast<std::uint8_t>(body_size >> 8);
    bytes_[3] = static_cast<std::uint8_t>(body_size);
    bytes_[4] = kAckType;
    bytes_[5] = kChannel;
    bytes_[6] = static_cast<std::uint8_t>(header_word >> 8);
    bytes_[7] = static_cast<std::uint8_t>(header_word);
    if (payload_size > 0) {
        std::memcpy(bytes_.data() + kHeaderSize, payload, payload_size);
    }
    size_ = packet_size;
    return true;
}

bool ArtemisRudpPacket::parse(const std::uint8_t *bytes, std::size_t size)
{
    size_ = 0;
    if (bytes == nullptr || size < kHeaderSize || size > bytes_.size() ||
        bytes[0] != 0xf1) {
        return false;
    }

    const std::size_t body_size =
        (static_cast<std::size_t>(bytes[2]) << 8) | bytes[3];
    if (body_size < 4 || body_size != size - 4 || bytes[4] != kAckType ||
        bytes[5] != kChannel) {
        return false;
    }

    std::memcpy(bytes_.data(), bytes, size);
    size_ = size;
    return true;
}

std::uint16_t ArtemisRudpPacket::header_word() const
{
    return static_cast<std::uint16_t>((bytes_[6] << 8) | bytes_[7]);
}

bool ArtemisRudpPacket::is_data() const
{
    return size_ >= kHeaderSize && bytes_[1] == kDataType;
}

bool ArtemisRudpPacket::is_ack() const
{
    if (size_ < kHeaderSize + 2 || bytes_[1] != kAckType) {
        return false;
    }
    const std::size_t count = header_word();
    return count > 0 && size_ - kHeaderSize == count * 2;
}

std::uint16_t ArtemisRudpPacket::index() const
{
    return header_word();
}

std::uint16_t ArtemisRudpPacket::ack_count() const
{
    return is_ack() ? header_word() : 0;
}

bool ArtemisRudpPacket::acknowledges(std::uint16_t index) const
{
    const std::size_t count = ack_count();
    for (std::size_t i = 0; i < count; i++) {
        const std::uint8_t *entry = bytes_.data() + kHeaderSize + i * 2;
        if (static_cast<std::uint16_t>((entry[0] << 8) | entry[1]) == index) {
            return true;
        }
    }
    return false;
}

const std::uint8_t *ArtemisRudpPacket::data() const
{
    return bytes_.data();
}

std::size_t ArtemisRudpPacket::size() const
{
    return size_;
}

bool ArtemisDataRequest::build(std::uint16_t index, const std::uint8_t *payload,
                               std::size_t payload_size)
{
    return ArtemisRudpPacket::build(kDataType, index, payload, payload_size);
}

bool ArtemisAckRequest::build(std::uint16_t received_index)
{
    const std::uint8_t payload[] = {
        static_cast<std::uint8_t>(received_index >> 8),
        static_cast<std::uint8_t>(received_index),
    };
    return ArtemisRudpPacket::build(kAckType, 1, payload, sizeof(payload));
}

bool ArtemisMagicRequest::build(Stage stage)
{
    static constexpr std::uint8_t kFirstPayload[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static constexpr std::uint8_t kSecondPayload[] = {0x00, 0x00};
    switch (stage) {
    case Stage::first:
        return ArtemisRudpPacket::build(kAckType, 3, kFirstPayload,
                                        sizeof(kFirstPayload));
    case Stage::second:
        return ArtemisRudpPacket::build(kAckType, 4, kSecondPayload,
                                        sizeof(kSecondPayload));
    }
    return false;
}

ArtemisHeartbeatPayload::ArtemisHeartbeatPayload()
{
    std::memcpy(bytes_.data(), kPrefix.data(), kPrefix.size());
    std::memcpy(bytes_.data() + kPrefix.size() + 1, kSuffix.data(),
                kSuffix.size());
}

const std::uint8_t *ArtemisHeartbeatPayload::next_command()
{
    command_count_ = static_cast<std::uint8_t>((command_count_ + 1) % 255);
    bytes_[kPrefix.size()] = command_count_;
    return bytes_.data();
}

std::size_t ArtemisHeartbeatPayload::size() const
{
    return bytes_.size();
}

std::uint8_t ArtemisHeartbeatPayload::command_count() const
{
    return command_count_;
}

}  // namespace camera
