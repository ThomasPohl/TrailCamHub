#include "camera_udp_packets.hpp"

#include <cstring>

namespace camera {

const std::uint8_t *CameraDiscoveryPacket::data() const
{
    return kBytes.data();
}

std::size_t CameraDiscoveryPacket::size() const
{
    return kBytes.size();
}

bool ArtemisRudpPacket::build(std::uint8_t type, std::uint8_t sequence,
                              const std::uint8_t *payload,
                              std::size_t payload_size)
{
    size_ = 0;
    const std::size_t body_size = payload_size + 4;
    const std::size_t packet_size = payload_size + kHeaderSize;
    if ((payload_size > 0 && payload == nullptr) ||
        packet_size > bytes_.size() || body_size > UINT16_MAX) {
        return false;
    }

    bytes_[0] = 0xf1;
    bytes_[1] = type;
    bytes_[2] = static_cast<std::uint8_t>(body_size >> 8);
    bytes_[3] = static_cast<std::uint8_t>(body_size);
    bytes_[4] = kAckType;
    bytes_[5] = 0;
    bytes_[6] = 0;
    bytes_[7] = sequence;
    if (payload_size > 0) {
        std::memcpy(bytes_.data() + kHeaderSize, payload, payload_size);
    }
    size_ = packet_size;
    return true;
}

bool ArtemisRudpPacket::parse(const std::uint8_t *bytes, std::size_t size)
{
    size_ = 0;
    if (bytes == nullptr || size < kHeaderSize ||
        size > bytes_.size() || bytes[0] != 0xf1) {
        return false;
    }

    const std::size_t body_size =
        (static_cast<std::size_t>(bytes[2]) << 8) | bytes[3];
    if (body_size < 4 || body_size != size - 4 ||
        bytes[4] != kAckType || bytes[5] != 0 || bytes[6] != 0) {
        return false;
    }

    std::memcpy(bytes_.data(), bytes, size);
    size_ = size;
    return true;
}

bool ArtemisRudpPacket::is_data() const
{
    return size_ >= kHeaderSize && bytes_[1] == kDataType;
}

bool ArtemisRudpPacket::is_ack() const
{
    if (size_ < 10 || bytes_[1] != kAckType) {
        return false;
    }

    const std::size_t payload_size = size_ - kHeaderSize;
    if (payload_size != 2 && payload_size != 4) {
        return false;
    }
    return bytes_[8] == 0 &&
           (payload_size == 2 || bytes_[10] == 0);
}

bool ArtemisRudpPacket::acknowledges(std::uint8_t sequence) const
{
    if (!is_ack()) {
        return false;
    }
    const std::size_t payload_size = size_ - kHeaderSize;
    if (bytes_[8] != 0 || bytes_[9] != sequence) {
        return false;
    }
    return payload_size == 2 ||
           (bytes_[10] == 0 && bytes_[11] == sequence);
}

std::uint8_t ArtemisRudpPacket::sequence() const
{
    return bytes_[7];
}

const std::uint8_t *ArtemisRudpPacket::data() const
{
    return bytes_.data();
}

std::size_t ArtemisRudpPacket::size() const
{
    return size_;
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
