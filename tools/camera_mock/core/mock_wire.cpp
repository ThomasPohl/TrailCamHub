#include "mock_wire.hpp"

namespace camera_mock::wire
{

namespace
{

std::uint16_t read_u16_be(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void append_u16_be(std::vector<std::uint8_t> &out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

}  // namespace

Packet parse(const std::uint8_t *bytes, std::size_t size)
{
    Packet packet;
    if (bytes == nullptr || size < 4 || bytes[0] != kMagic) {
        return packet;
    }
    packet.type = bytes[1];
    const std::size_t length = read_u16_be(bytes + 2);
    if (length + 4 != size) {
        return packet;
    }

    switch (packet.type) {
    case kDiscoveryRequest:
        packet.kind = Packet::Kind::discovery_request;
        return packet;
    case kPreLogin:
        packet.kind = Packet::Kind::pre_login;
        packet.payload = bytes + 4;
        packet.payload_size = size - 4;
        return packet;
    case kAlive:
        packet.kind = length == 0 ? Packet::Kind::alive : Packet::Kind::other;
        return packet;
    case kAliveAck:
        packet.kind =
            length == 0 ? Packet::Kind::alive_ack : Packet::Kind::other;
        return packet;
    case kClose:
        packet.kind = length == 0 ? Packet::Kind::close : Packet::Kind::other;
        return packet;
    case kData:
    case kAck:
        break;
    default:
        packet.kind = Packet::Kind::other;
        return packet;
    }

    if (size < kDrwHeaderSize || bytes[4] != 0xd1) {
        packet.kind = Packet::Kind::other;
        return packet;
    }
    packet.channel = bytes[5];
    const std::uint16_t word = read_u16_be(bytes + 6);
    if (packet.type == kData) {
        packet.kind = Packet::Kind::data;
        packet.index = word;
        packet.payload = bytes + kDrwHeaderSize;
        packet.payload_size = size - kDrwHeaderSize;
        return packet;
    }
    packet.kind = Packet::Kind::ack;
    const std::size_t present = (size - kDrwHeaderSize) / 2;
    const std::size_t count = word < present ? word : present;
    for (std::size_t i = 0; i < count; i++) {
        packet.acked.push_back(read_u16_be(bytes + kDrwHeaderSize + i * 2));
    }
    return packet;
}

std::vector<std::uint8_t> control(std::uint8_t type)
{
    return {kMagic, type, 0x00, 0x00};
}

std::vector<std::uint8_t> data(std::uint16_t index, const std::uint8_t *payload,
                               std::size_t size)
{
    std::vector<std::uint8_t> out;
    out.reserve(kDrwHeaderSize + size);
    out.push_back(kMagic);
    out.push_back(kData);
    append_u16_be(out, static_cast<std::uint16_t>(size + 4));
    out.push_back(0xd1);
    out.push_back(0x00);
    append_u16_be(out, index);
    out.insert(out.end(), payload, payload + size);
    return out;
}

std::vector<std::uint8_t> ack(std::uint16_t index)
{
    std::vector<std::uint8_t> out = {kMagic, kAck, 0x00, 0x06,
                                     0xd1,   0x00, 0x00, 0x01};
    append_u16_be(out, index);
    return out;
}

}  // namespace camera_mock::wire
