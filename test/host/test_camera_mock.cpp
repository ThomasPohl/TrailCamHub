// Camera mock (tools/camera_mock, #122). The session test drives the mock
// with the hub's own client code (pppp::Session, ARTEMIS requests, the
// response parsers): the mock does not use that code to answer, so the test
// checks that the two sides interoperate.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "artemis_message.hpp"
#include "artemis_reassembler.hpp"
#include "ble_frames.hpp"
#include "camera_prelogin.hpp"
#include "camera_udp_packets.hpp"
#include "capture_fixtures.hpp"
#include "device_info.hpp"
#include "media_list.hpp"
#include "mock_camera.hpp"
#include "mock_wire.hpp"
#include "pppp_session.hpp"

using namespace camera_mock;

namespace
{

std::string decrypt(const fixtures::CapturedResponse &response)
{
    std::string json;
    REQUIRE(camera::artemis::decode(camera::artemis::command_cipher(),
                                    response.base64, json));
    return json;
}

using Datagram = std::vector<std::uint8_t>;

// Datagrams in one direction, collected for the other side.
struct Pipe final : Output, camera::pppp::Transport {
    std::vector<Datagram> packets;

    void send(const std::uint8_t *data, std::size_t size) override
    {
        packets.emplace_back(data, data + size);
    }
    bool send(const std::uint8_t *data, std::size_t size, const char *) override
    {
        packets.emplace_back(data, data + size);
        return true;
    }
    std::vector<Datagram> take()
    {
        std::vector<Datagram> out;
        out.swap(packets);
        return out;
    }
};

// The hub side: the firmware's reliable-UDP session and ARTEMIS client code.
struct Hub {
    Pipe to_camera;
    camera::pppp::Session session{to_camera};
    camera::artemis::Aes128 cipher = camera::artemis::command_cipher();
    camera::artemis::Reassembler reassembler;
    std::vector<std::string> responses;
    std::vector<std::uint16_t> data_indices;
    bool closed = false;

    void receive(const std::vector<Datagram> &packets)
    {
        for (const Datagram &packet : packets) {
            const auto event =
                session.handle_packet(packet.data(), packet.size());
            if (event.type == camera::pppp::Session::Event::Type::close) {
                closed = true;
            }
            if (event.type != camera::pppp::Session::Event::Type::data) {
                continue;
            }
            data_indices.push_back(event.index);
            const auto status =
                reassembler.feed(event.payload, event.payload_size);
            if (status == camera::artemis::Reassembler::Status::complete) {
                camera::artemis::Message message;
                REQUIRE(camera::artemis::parse(
                    cipher, reassembler.message().data(),
                    reassembler.message().size(), message));
                CHECK(message.msg_type == camera::artemis::kMsgResponse);
                responses.push_back(message.json);
                reassembler.reset();
            } else if (status ==
                       camera::artemis::Reassembler::Status::invalid) {
                reassembler.reset();  // the camera's "ACK" DATA
            }
        }
    }

    void command(std::uint32_t seq, const std::string &json,
                 std::int64_t now_ms)
    {
        const std::vector<std::uint8_t> request =
            camera::artemis::build_request(cipher, seq, json);
        REQUIRE_FALSE(request.empty());
        REQUIRE(session.send_data(request.data(), request.size(), now_ms * 1000,
                                  "test"));
    }
};

struct Bench {
    Pipe to_hub;
    std::vector<std::string> log;
    Camera camera{to_hub, [this](LogLevel, const std::string &text) {
                      log.push_back(text);
                  }};
    Hub hub;
    std::int64_t now_ms = 1000;

    // Lets packets go back and forth (and the camera's timers run).
    void run(int steps = 10)
    {
        for (int i = 0; i < steps; i++) {
            for (const Datagram &packet : hub.to_camera.take()) {
                camera.handle_packet(packet.data(), packet.size(), now_ms);
            }
            camera.poll(now_ms);
            hub.receive(to_hub.take());
            hub.session.poll(now_ms * 1000);
            now_ms += 10;
        }
    }

    void discover_and_login()
    {
        const camera::CameraDiscoveryRequest discovery;
        hub.to_camera.send(discovery.data(), discovery.size(), "discovery");
        run(1);
        hub.session.reset();
        std::uint8_t nonce[camera::kPreLoginNonceSize] = {1, 2, 3, 4,
                                                          5, 6, 7, 8};
        std::uint8_t body[camera::kPreLoginMaxBodySize];
        const std::size_t body_size = camera::build_prelogin_body(
            1770401530, nonce, body, sizeof(body), nullptr);
        camera::PreLoginRequest pre_login;
        REQUIRE(pre_login.build(body, body_size));
        hub.to_camera.send(pre_login.data(), pre_login.size(), "PRE_LOGIN");
        run(5);
        hub.command(1, camera::artemis::build_login_json(0, "", ""), now_ms);
        run(10);
    }
};

}  // namespace

TEST_CASE("camera mock: the captured fixtures decode")
{
    const std::string login = decrypt(fixtures::kLoginResponse);
    CHECK(login == "{\"cmdId\":0,\"result\":0,\"token\":37817828,"
                   "\"bat_percent\":100,\"errorMsg\":\"Success\"}");
    std::int64_t token = 0;
    REQUIRE(find_int(login, "token", token));
    CHECK(token == fixtures::kCapturedToken);

    camera::artemis::DeviceInfo info;
    REQUIRE(camera::artemis::parse_device_info(
        decrypt(fixtures::kDeviceInfoResponse), info));
    CHECK(info.dev_id == ble::format_mac(fixtures::kDevId));
    CHECK(info.model_name == "PH970S");
    CHECK(std::string(fixtures::kBleName) ==
          "TC100_" + info.dev_id.substr(12, 2) + info.dev_id.substr(15, 2));

    for (int page = 0; page < 2; page++) {
        const auto &fixture = page == 0 ? fixtures::kMediaListPage0Response
                                        : fixtures::kMediaListPage1Response;
        CHECK(fixture.padding == fixtures::kCapturedPadding);
        camera::artemis::MediaList list;
        REQUIRE(camera::artemis::parse_media_list(decrypt(fixture), list));
        CHECK(list.page_no == page);
        CHECK(list.files.size() == fixtures::kCapturedItemsPerPage);
    }
}

TEST_CASE("camera mock: wire format")
{
    // The hub's Magic packets: ACK-shaped, with counts that do not match.
    const std::uint8_t magic_one[] = {0xf1, 0xd1, 0x00, 0x0a, 0xd1, 0x00, 0x00,
                                      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    wire::Packet packet = wire::parse(magic_one, sizeof(magic_one));
    CHECK(packet.kind == wire::Packet::Kind::ack);
    CHECK(packet.acked == std::vector<std::uint16_t>{0, 0, 0});
    const std::uint8_t magic_two[] = {0xf1, 0xd1, 0x00, 0x06, 0xd1,
                                      0x00, 0x00, 0x04, 0x00, 0x00};
    packet = wire::parse(magic_two, sizeof(magic_two));
    CHECK(packet.kind == wire::Packet::Kind::ack);
    CHECK(packet.acked == std::vector<std::uint16_t>{0});

    // The camera's first DATA and its ACK, as captured.
    CHECK(wire::data(0, fixtures::kFirstDataPayload, 3) ==
          std::vector<std::uint8_t>{0xf1, 0xd0, 0x00, 0x07, 0xd1, 0x00, 0x00,
                                    0x00, 'A', 'C', 'K'});
    CHECK(wire::ack(5) == std::vector<std::uint8_t>{0xf1, 0xd1, 0x00, 0x06,
                                                    0xd1, 0x00, 0x00, 0x01,
                                                    0x00, 0x05});
    CHECK(wire::control(wire::kAlive) ==
          std::vector<std::uint8_t>{0xf1, 0xe0, 0x00, 0x00});

    // A wrong length field is not a packet.
    const std::uint8_t short_alive[] = {0xf1, 0xe0, 0x00, 0x02};
    CHECK(wire::parse(short_alive, sizeof(short_alive)).kind ==
          wire::Packet::Kind::invalid);
}

TEST_CASE("camera mock: ARTEMIS response framing")
{
    // Login response as captured: PayloadLen 0x81 = 108 Base64 + 21.
    const auto message = artemis_response(0x21, fixtures::kLoginResponse.base64,
                                          fixtures::kLoginResponse.padding);
    REQUIRE(message.size() == 20 + 129);
    CHECK(std::vector<std::uint8_t>(message.begin(), message.begin() + 20) ==
          std::vector<std::uint8_t>{'A', 'R', 'T',  'E', 'M', 'I',  'S',
                                    0,   3,   0,    0,   0,   0x21, 0,
                                    0,   0,   0x81, 0,   0,   0});
    CHECK(message.back() == 0);
}

TEST_CASE("camera mock: BLE frames")
{
    const auto adv = ble::advertising_data(
        fixtures::kDevId, fixtures::kProductId, fixtures::kBleName);
    // Byte for byte the advertisement in the btsnoop capture.
    const std::vector<std::uint8_t> captured = {
        0x02, 0x01, 0x06, 0x03, 0x03, 0x01, 0x88, 0x0b, 0xff, 0x87, 0x19,
        0xc6, 0x1e, 0x0d, 0xe0, 0x0c, 0xfb, 0x00, 0x68, 0x0b, 0x09, 'T',
        'C',  '1',  '0',  '0',  '_',  '0',  'C',  'F',  'B'};
    CHECK(adv == captured);

    const std::uint8_t bt_key_on[] = {'B', 'T', '_', 'K', 'E',
                                      'Y', '_', 'O', 'N'};
    CHECK(ble::is_wake_command(bt_key_on, sizeof(bt_key_on)));
    std::uint8_t app_request[20] = {0x13, 0x57, 0x01, 0x00};
    CHECK(ble::is_wake_command(app_request, sizeof(app_request)));
    CHECK_FALSE(ble::is_wake_command(bt_key_on, 5));

    const std::uint8_t bssid[6] = {0x1c, 0x4e, 0xa2, 0x7e, 0x5d, 0xbd};
    CHECK(ble::ap_ssid(bssid) == "TrailCam_5DBD");
    const auto frame = ble::credentials_frame(
        ble::ap_ssid(bssid), ble::format_mac(bssid), "39736428");
    const std::string json = "{\"ret\":0,\"ssid\":\"TrailCam_5DBD\","
                             "\"bssid\":\"1C:4E:A2:7E:5D:BD\","
                             "\"pwd\":\"39736428\"}";
    // The capture: 13 57 01 00 4d 00 00 00 and 77 bytes of JSON.
    REQUIRE(frame.size() == 8 + 77);
    CHECK(std::vector<std::uint8_t>(frame.begin(), frame.begin() + 8) ==
          std::vector<std::uint8_t>{0x13, 0x57, 0x01, 0x00, 0x4d, 0, 0, 0});
    CHECK(std::string(frame.begin() + 8, frame.end()) == json);
    const auto chunks = ble::split_notifications(frame, 20);
    REQUIRE(chunks.size() == 5);
    CHECK(chunks[4].size() == 5);
}

TEST_CASE("camera mock: a hub session against the mock")
{
    Bench bench;
    bench.discover_and_login();

    // Discovery reply, then the first DATA ("ACK", index 0) and the login
    // response (index 1).
    CHECK(bench.camera.state() == Camera::State::session);
    CHECK(bench.camera.logged_in());
    CHECK(bench.camera.stats().pre_logins == 1);
    REQUIRE(bench.hub.responses.size() == 1);
    std::int64_t token = 0;
    REQUIRE(find_int(bench.hub.responses[0], "token", token));
    CHECK(token == fixtures::kCapturedToken);

    // A heartbeat is acknowledged, never answered.
    camera::artemis::Heartbeat heartbeat;
    const auto beat = heartbeat.next(bench.hub.cipher);
    REQUIRE(bench.hub.session.send_data(beat.data(), beat.size(),
                                        bench.now_ms * 1000, "heartbeat"));
    bench.run();
    CHECK(bench.hub.session.outstanding_count() == 0);
    CHECK(bench.hub.responses.size() == 1);

    const std::string t = ",\"token\":" + std::to_string(token) + "}";
    bench.hub.command(2, "{\"cmdId\":512" + t, bench.now_ms);
    bench.run();
    REQUIRE(bench.hub.responses.size() == 2);
    camera::artemis::DeviceInfo info;
    REQUIRE(camera::artemis::parse_device_info(bench.hub.responses[1], info));
    CHECK(info.dev_id == "C6:1E:0D:E0:0C:FB");

    for (int page = 0; page < 3; page++) {
        bench.hub.command(3 + page,
                          "{\"cmdId\":768,\"itemCntPerPage\":45,\"pageNo\":" +
                              std::to_string(page) + t,
                          bench.now_ms);
        bench.run(20);
        REQUIRE(bench.hub.responses.size() == 3 + page);
        camera::artemis::MediaList list;
        REQUIRE(camera::artemis::parse_media_list(bench.hub.responses.back(),
                                                  list));
        CHECK(list.page_no == page);
        CHECK(list.files.size() == (page < 2 ? 45u : 0u));
    }

    // Each DATA index arrived once and in order: 0 "ACK", 1 login, 2-4 the
    // device info (2945 bytes), then 7 fragments per captured page.
    REQUIRE(bench.hub.data_indices.size() == 1 + 1 + 3 + 7 + 7 + 1);
    for (std::size_t i = 0; i < bench.hub.data_indices.size(); i++) {
        CHECK(bench.hub.data_indices[i] == i);
    }
    CHECK(bench.camera.outstanding() == 0);

    // The hub's CLOSE ends the session.
    const camera::ControlRequest close(camera::PacketType::close);
    bench.hub.to_camera.send(close.data(), close.size(), "CLOSE");
    bench.run(1);
    CHECK(bench.camera.state() == Camera::State::idle);
}

TEST_CASE("camera mock: commands that the camera does not answer")
{
    Bench bench;
    bench.discover_and_login();
    REQUIRE(bench.hub.responses.size() == 1);

    // 768 without paging parameters, and a wrong token.
    bench.hub.command(2, "{\"cmdId\":768,\"token\":37817828}", bench.now_ms);
    bench.hub.command(3, "{\"cmdId\":512,\"token\":1}", bench.now_ms);
    bench.run();
    CHECK(bench.hub.responses.size() == 1);
    // Acknowledged all the same.
    CHECK(bench.hub.session.outstanding_count() == 0);
}

TEST_CASE("camera mock: first DATA without PRE_LOGIN, ALIVE, retransmission")
{
    Bench bench;
    const camera::CameraDiscoveryRequest discovery;
    bench.hub.to_camera.send(discovery.data(), discovery.size(), "discovery");
    bench.run(1);
    REQUIRE(bench.to_hub.packets.empty());
    CHECK(bench.camera.state() == Camera::State::discovered);

    // No PRE_LOGIN: the "ACK" DATA comes after the delay on its own. Lost
    // packets (the hub does not read them) are sent again.
    for (int i = 0; i < 50; i++) {
        bench.camera.poll(bench.now_ms);
        bench.now_ms += 10;
    }
    CHECK(bench.camera.state() == Camera::State::session);
    CHECK(bench.camera.stats().retransmissions > 0);
    CHECK(bench.camera.outstanding() == 1);

    // ALIVE every 2 s; the hub's ALIVE is answered.
    bench.to_hub.take();
    bench.now_ms += 2000;
    bench.camera.poll(bench.now_ms);
    const std::uint8_t alive[] = {0xf1, 0xe0, 0x00, 0x00};
    bench.camera.handle_packet(alive, sizeof(alive), bench.now_ms);
    const auto sent = bench.to_hub.take();
    bool sent_alive = false;
    bool sent_alive_ack = false;
    for (const Datagram &packet : sent) {
        sent_alive |= packet == Datagram{0xf1, 0xe0, 0x00, 0x00};
        sent_alive_ack |= packet == Datagram{0xf1, 0xe1, 0x00, 0x00};
    }
    CHECK(sent_alive);
    CHECK(sent_alive_ack);

    // Silence from the hub drops the session.
    bench.now_ms += 31000;
    bench.camera.poll(bench.now_ms);
    CHECK(bench.camera.state() == Camera::State::idle);
}
