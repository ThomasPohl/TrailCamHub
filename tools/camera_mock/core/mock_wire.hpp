#pragma once

// Camera side of the PPPP/CS2 framing, written for the mock on its own (not
// shared with the hub's camera_udp_packets) so that a framing mistake in the
// hub is not repeated here. Layouts as seen in the captures:
//
//   control  F1 <type> 00 00
//   DATA     F1 D0 <len:2 BE> D1 <channel> <index:2 BE> <payload>
//   ACK      F1 D1 <len:2 BE> D1 <channel> <count:2 BE> <index:2 BE>*count
//
// `len` counts the bytes after the length field. Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera_mock::wire
{

constexpr std::uint8_t kMagic = 0xf1;
constexpr std::uint8_t kDiscoveryRequest = 0x41;
constexpr std::uint8_t kDiscoveryReply = 0x42;
constexpr std::uint8_t kData = 0xd0;
constexpr std::uint8_t kAck = 0xd1;
constexpr std::uint8_t kAlive = 0xe0;
constexpr std::uint8_t kAliveAck = 0xe1;
constexpr std::uint8_t kClose = 0xf0;
constexpr std::uint8_t kPreLogin = 0xf9;

constexpr std::size_t kDrwHeaderSize = 8;
// Largest DATA payload the camera sends: big responses are split into DATA
// packets of 1024 payload bytes (1032-byte datagrams) in every capture.
constexpr std::size_t kMaxDataPayload = 1024;

struct Packet {
    enum class Kind {
        invalid,
        discovery_request,
        pre_login,
        data,
        ack,
        alive,
        alive_ack,
        close,
        other,
    };

    Kind kind = Kind::invalid;
    std::uint8_t type = 0;
    // DATA and ACK: the channel (0 = commands; others carry bulk streams).
    std::uint8_t channel = 0;
    // DATA: the packet index.
    std::uint16_t index = 0;
    // DATA: the payload; PRE_LOGIN: the bytes after the 4-byte header.
    const std::uint8_t *payload = nullptr;
    std::size_t payload_size = 0;
    // ACK: the acknowledged indices. Clients send ACKs whose count does not
    // match their length (the hub's Magic packets, the app's padded ACKs),
    // so only the entries that are really there are read.
    std::vector<std::uint16_t> acked;
};

Packet parse(const std::uint8_t *bytes, std::size_t size);

std::vector<std::uint8_t> control(std::uint8_t type);
std::vector<std::uint8_t> data(std::uint16_t index, const std::uint8_t *payload,
                               std::size_t size);
// Single-index ACK on channel 0, the camera's usual form:
// F1 D1 00 06 D1 00 00 01 <index:2>.
std::vector<std::uint8_t> ack(std::uint16_t index);

}  // namespace camera_mock::wire
