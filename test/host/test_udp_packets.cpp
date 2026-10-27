#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "camera_udp_packets.hpp"

using namespace camera;

namespace
{

std::vector<std::uint8_t> bytes_of(const UdpRequest &request)
{
    return std::vector<std::uint8_t>(request.data(),
                                     request.data() + request.size());
}

// Packets below were captured from a TC100 camera.
const std::uint8_t kCameraDataAck[] = {0xf1, 0xd0, 0x00, 0x07, 0xd1, 0x00,
                                       0x00, 0x00, 0x41, 0x43, 0x4b};
const std::uint8_t kCameraAckOfIndex5[] = {0xf1, 0xd1, 0x00, 0x06, 0xd1,
                                           0x00, 0x00, 0x01, 0x00, 0x05};
const std::uint8_t kCameraDiscoveryReply[] = {
    0xf1, 0x42, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
};

}  // namespace

TEST_CASE("discovery request matches the reference bytes")
{
    const CameraDiscoveryRequest request;
    const std::vector<std::uint8_t> expected = {
        0xf1, 0x41, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
    };
    CHECK(bytes_of(request) == expected);
}

TEST_CASE("control packets")
{
    PacketType type;
    const std::uint8_t alive[] = {0xf1, 0xe0, 0x00, 0x00};
    const std::uint8_t close[] = {0xf1, 0xf0, 0x00, 0x00};
    REQUIRE(parse_control_packet(alive, sizeof(alive), &type));
    CHECK(type == PacketType::alive);
    REQUIRE(parse_control_packet(close, sizeof(close), &type));
    CHECK(type == PacketType::close);
    CHECK_FALSE(
        parse_control_packet(kCameraDataAck, sizeof(kCameraDataAck), &type));
    CHECK_FALSE(parse_control_packet(kCameraDiscoveryReply,
                                     sizeof(kCameraDiscoveryReply), &type));

    const ControlRequest alive_ack(PacketType::alive_ack);
    CHECK(bytes_of(alive_ack) ==
          std::vector<std::uint8_t>{0xf1, 0xe1, 0x00, 0x00});
}

TEST_CASE("parses camera DATA and ACK packets")
{
    ArtemisRudpPacket packet;
    REQUIRE(packet.parse(kCameraDataAck, sizeof(kCameraDataAck)));
    CHECK(packet.is_data());
    CHECK_FALSE(packet.is_ack());
    CHECK(packet.index() == 0);

    REQUIRE(packet.parse(kCameraAckOfIndex5, sizeof(kCameraAckOfIndex5)));
    CHECK(packet.is_ack());
    CHECK(packet.ack_count() == 1);
    CHECK(packet.acknowledges(5));
    CHECK_FALSE(packet.acknowledges(4));

    CHECK_FALSE(
        packet.parse(kCameraDiscoveryReply, sizeof(kCameraDiscoveryReply)));
}

TEST_CASE("ACK lists and inconsistent counts")
{
    ArtemisRudpPacket packet;
    const std::uint8_t two[] = {0xf1, 0xd1, 0x00, 0x08, 0xd1, 0x00,
                                0x00, 0x02, 0x01, 0x02, 0x00, 0x07};
    REQUIRE(packet.parse(two, sizeof(two)));
    CHECK(packet.acknowledges(0x0102));
    CHECK(packet.acknowledges(7));

    const std::uint8_t wrong_count[] = {0xf1, 0xd1, 0x00, 0x06, 0xd1,
                                        0x00, 0x00, 0x03, 0x00, 0x05};
    REQUIRE(packet.parse(wrong_count, sizeof(wrong_count)));
    CHECK_FALSE(packet.is_ack());
    CHECK_FALSE(packet.acknowledges(5));
}

TEST_CASE("rejects packets with an inconsistent length")
{
    ArtemisRudpPacket packet;
    const std::uint8_t wrong_length[] = {0xf1, 0xd0, 0x00, 0x08, 0xd1, 0x00,
                                         0x00, 0x00, 0x41, 0x43, 0x4b};
    CHECK_FALSE(packet.parse(wrong_length, sizeof(wrong_length)));
    const std::uint8_t other_channel[] = {0xf1, 0xd0, 0x00, 0x07, 0xd1, 0x01,
                                          0x00, 0x00, 0x41, 0x43, 0x4b};
    CHECK_FALSE(packet.parse(other_channel, sizeof(other_channel)));
}

TEST_CASE("builds ACKs in the camera's format")
{
    ArtemisAckRequest ack;
    REQUIRE(ack.build(5));
    CHECK(bytes_of(ack) ==
          std::vector<std::uint8_t>(kCameraAckOfIndex5,
                                    kCameraAckOfIndex5 +
                                        sizeof(kCameraAckOfIndex5)));
    REQUIRE(ack.build(0x1234));
    CHECK(bytes_of(ack).back() == 0x34);
}

TEST_CASE("builds DATA packets with 16-bit indices")
{
    const std::uint8_t payload[] = {'A', 'C', 'K'};
    ArtemisDataRequest data;
    REQUIRE(data.build(0, payload, sizeof(payload)));
    CHECK(bytes_of(data) ==
          std::vector<std::uint8_t>(kCameraDataAck,
                                    kCameraDataAck + sizeof(kCameraDataAck)));
    REQUIRE(data.build(0x0102, payload, sizeof(payload)));
    CHECK(data.data()[6] == 0x01);
    CHECK(data.data()[7] == 0x02);
}

TEST_CASE("Magic packets keep their bytes")
{
    ArtemisMagicRequest magic;
    REQUIRE(magic.build(ArtemisMagicRequest::Stage::first));
    CHECK(bytes_of(magic) ==
          std::vector<std::uint8_t>{0xf1, 0xd1, 0x00, 0x0a, 0xd1, 0x00, 0x00,
                                    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    REQUIRE(magic.build(ArtemisMagicRequest::Stage::second));
    CHECK(bytes_of(magic) == std::vector<std::uint8_t>{0xf1, 0xd1, 0x00, 0x06,
                                                       0xd1, 0x00, 0x00, 0x04,
                                                       0x00, 0x00});
}

TEST_CASE("heartbeat payload matches the captured heartbeat")
{
    ArtemisHeartbeatPayload heartbeat;
    const std::uint8_t *payload = heartbeat.next_command();
    CHECK(heartbeat.command_count() == 2);
    const std::vector<std::uint8_t> expected = {
        0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x4d, 0x7a, 0x6c, 0x42,
        0x33, 0x36, 0x58, 0x2f, 0x49, 0x56, 0x6f, 0x38, 0x5a, 0x7a, 0x49, 0x35,
        0x72, 0x47, 0x39, 0x6a, 0x31, 0x77, 0x3d, 0x3d, 0x00,
    };
    REQUIRE(heartbeat.size() == expected.size());
    CHECK(std::vector<std::uint8_t>(payload, payload + heartbeat.size()) ==
          expected);
    heartbeat.next_command();
    CHECK(heartbeat.command_count() == 3);
}

TEST_CASE("PRE_LOGIN and ARTEMIS command framing")
{
    std::uint8_t body[76] = {};
    PreLoginRequest prelogin;
    REQUIRE(prelogin.build(body, sizeof(body)));
    CHECK(prelogin.size() == 80);
    CHECK(prelogin.data()[0] == 0xf1);
    CHECK(prelogin.data()[1] == 0xf9);
    CHECK(prelogin.data()[2] == 0x00);
    CHECK(prelogin.data()[3] == 0x4c);

    ArtemisCommandRequest command;
    REQUIRE(command.build(7, "QUJD", 4));
    CHECK(command.size() == ArtemisCommandRequest::kHeaderSize + 5);
    CHECK(command.data()[8] == 2);
    CHECK(command.data()[12] == 7);
    CHECK(command.data()[16] == 4);
    CHECK(command.data()[command.size() - 1] == 0);
}
