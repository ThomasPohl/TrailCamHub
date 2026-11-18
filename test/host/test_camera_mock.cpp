// Camera mock (tools/camera_mock, #122). The session test drives the mock
// with the hub's own client code (pppp::Session, ARTEMIS requests, the
// response parsers): the mock does not use that code to answer, so the test
// checks that the two sides interoperate.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "artemis_message.hpp"
#include "artemis_reassembler.hpp"
#include "ble_frames.hpp"
#include "bulk_reassembler.hpp"
#include "camera_action.hpp"
#include "camera_prelogin.hpp"
#include "camera_udp_packets.hpp"
#include "capture_fixtures.hpp"
#include "device_info.hpp"
#include "media_list.hpp"
#include "media_transfer.hpp"
#include "mock_camera.hpp"
#include "mock_wire.hpp"
#include "playback_record.hpp"
#include "playback_stream.hpp"
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
    // Bulk channel (#122 stage 2/3): one reassembler per subtype, as in
    // the firmware; every frame is acknowledged.
    camera::artemis::BulkReassembler thumbnail_bulk;
    camera::artemis::BulkReassembler photo_bulk;
    struct Record {
        std::uint8_t subtype = 0;
        camera::artemis::MediaRecordHeader header;
        std::vector<std::uint8_t> file;
    };
    std::vector<Record> records;
    unsigned bulk_frames = 0;
    bool ack_bulk = true;
    // Playback stream (subtype 0x02): frames as they arrived.
    struct PlaybackFrame {
        std::uint16_t seq = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<PlaybackFrame> playback_frames;

    void receive_bulk(const Datagram &packet)
    {
        const std::uint8_t subtype = packet[5];
        const std::uint16_t seq =
            static_cast<std::uint16_t>((packet[6] << 8) | packet[7]);
        bulk_frames++;
        if (ack_bulk) {
            camera::ArtemisBulkAckRequest ack;
            REQUIRE(ack.build(subtype, &seq, 1));
            to_camera.send(ack.data(), ack.size(), "BULK ACK");
        }
        if (subtype == 0x02) {
            playback_frames.push_back(
                {seq,
                 std::vector<std::uint8_t>(packet.begin() + 8, packet.end())});
            return;
        }
        camera::artemis::BulkReassembler &bulk =
            subtype == 0x04 ? thumbnail_bulk : photo_bulk;
        const auto status =
            bulk.feed(seq, packet.data() + 8, packet.size() - 8);
        if (status == camera::artemis::BulkReassembler::Status::complete) {
            Record record;
            record.subtype = subtype;
            std::size_t offset = 0;
            std::size_t size = 0;
            REQUIRE(camera::artemis::parse_media_record(
                bulk.payload(), bulk.record_len(), record.header, offset,
                size));
            record.file.assign(bulk.payload() + offset,
                               bulk.payload() + offset + size);
            records.push_back(std::move(record));
            bulk.reset();
        } else if (status ==
                   camera::artemis::BulkReassembler::Status::invalid) {
            bulk.reset();
        }
    }

    void receive(const std::vector<Datagram> &packets)
    {
        for (const Datagram &packet : packets) {
            if (packet.size() > 8 && packet[0] == 0xf1 && packet[1] == 0xd0 &&
                packet[5] != 0) {
                receive_bulk(packet);
                continue;
            }
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
    Camera camera;
    Hub hub;
    std::int64_t now_ms = 1000;

    explicit Bench(const Options &options = Options())
        : camera(
              to_hub,
              [this](LogLevel, const std::string &text) {
                  log.push_back(text);
              },
              options)
    {
    }

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

TEST_CASE("camera mock: camera actions (#122 stage 3)")
{
    using camera::action::interpret;
    using camera::action::Type;
    Bench bench;
    bench.discover_and_login();
    REQUIRE(bench.hub.responses.size() == 1);
    const std::string t = ",\"token\":37817828}";
    std::uint32_t seq = 2;
    auto ask = [&](const std::string &json) {
        bench.hub.command(seq++, json, bench.now_ms);
        bench.run();
        REQUIRE(bench.hub.responses.size() == seq - 1);
        return bench.hub.responses.back();
    };

    // The hub reads each answer with its own rules (camera_action.cpp).
    CHECK(interpret(Type::photo, ask("{\"cmdId\":641" + t)).done);
    CHECK(bench.camera.stats().photos == 1);
    CHECK(interpret(Type::video_start, ask("{\"cmdId\":643" + t)).done);
    CHECK(bench.camera.recording());
    CHECK(interpret(Type::video_stop, ask("{\"cmdId\":644" + t)).done);
    CHECK_FALSE(bench.camera.recording());

    const std::string file =
        "{\"cmdId\":773,\"fileType\":1,\"dirNum\":100,\"mediaNum\":36" + t;
    CHECK(interpret(Type::delete_media, ask(file)).done);
    const auto again = interpret(Type::delete_media, ask(file));
    CHECK_FALSE(again.done);
    CHECK(again.code == -3);  // no such file
    CHECK_FALSE(interpret(Type::delete_media, ask("{\"cmdId\":773" + t)).done);
    CHECK(bench.camera.stats().deletions == 1);

    CHECK(interpret(Type::format_sd, ask("{\"cmdId\":518" + t)).done);
    CHECK(bench.camera.formatted());
    camera::artemis::MediaList list;
    REQUIRE(camera::artemis::parse_media_list(
        ask("{\"cmdId\":768,\"itemCntPerPage\":45,\"pageNo\":0" + t), list));
    CHECK(list.files.empty());
    // After the format the file can be "deleted" again (the card is empty).
    CHECK(interpret(Type::delete_media, ask(file)).done);
}

TEST_CASE("camera mock: fault switches (#122 stage 2)")
{
    Faults faults;
    CHECK(describe(faults) == "none");
    faults.drop_percent = 30;
    faults.battery_percent = 15;
    faults.sd_free_mb = 120;
    CHECK(describe(faults) == "drop 30 %, battery 15 %, SD free 120 MB");
    CHECK(patch_device_info("{\"batPercent\":100,\"sdFreeMB\":20617}",
                            faults) == "{\"batPercent\":15,\"sdFreeMB\":120}");

    SUBCASE("battery and SD card in the device info")
    {
        Bench bench;
        Faults low;
        low.battery_percent = 15;
        low.sd_free_mb = 120;
        bench.camera.set_faults(low);
        bench.discover_and_login();
        bench.hub.command(2, "{\"cmdId\":512,\"token\":37817828}",
                          bench.now_ms);
        bench.run();
        REQUIRE(bench.hub.responses.size() == 2);
        camera::artemis::DeviceInfo info;
        REQUIRE(
            camera::artemis::parse_device_info(bench.hub.responses[1], info));
        CHECK(info.dev_id == "C6:1E:0D:E0:0C:FB");
        std::int64_t value = 0;
        REQUIRE(find_int(bench.hub.responses[1], "batPercent", value));
        CHECK(value == 15);
        REQUIRE(find_int(bench.hub.responses[1], "sdFreeMB", value));
        CHECK(value == 120);
    }

    SUBCASE("dropped and duplicated DATA: the session still completes")
    {
        // Window 8: lost packets are overtaken by later ones; the hub
        // waits for the missing one (#313).
        Bench bench;
        Faults lossy;
        lossy.drop_percent = 40;
        lossy.duplicate_percent = 20;
        bench.camera.set_faults(lossy);
        bench.discover_and_login();
        bench.run(200);
        REQUIRE(bench.hub.responses.size() == 1);
        bench.hub.command(2, "{\"cmdId\":512,\"token\":37817828}",
                          bench.now_ms);
        bench.run(1500);
        for (int page = 0; page < 2; page++) {
            bench.hub.command(
                3 + page,
                "{\"cmdId\":768,\"itemCntPerPage\":45,\"pageNo\":" +
                    std::to_string(page) + ",\"token\":37817828}",
                bench.now_ms);
            bench.run(1500);
        }
        CHECK(bench.hub.responses.size() == 4);
        CHECK(bench.camera.stats().dropped > 0);
        CHECK(bench.camera.stats().duplicated > 0);
        CHECK(bench.camera.stats().retransmissions > 0);
    }

    SUBCASE("mute: no answer after the login")
    {
        Bench bench;
        Faults mute;
        mute.mute = true;
        bench.camera.set_faults(mute);
        bench.discover_and_login();
        REQUIRE(bench.hub.responses.size() == 1);  // the login is answered
        bench.hub.command(2, "{\"cmdId\":512,\"token\":37817828}",
                          bench.now_ms);
        bench.run();
        CHECK(bench.hub.responses.size() == 1);
        CHECK(bench.camera.stats().muted == 1);
    }

    SUBCASE("late ACKs keep the hub's DATA outstanding")
    {
        Bench bench;
        bench.discover_and_login();
        Faults late;
        late.ack_delay_ms = 500;
        bench.camera.set_faults(late);
        bench.hub.command(2, "{\"cmdId\":512,\"token\":37817828}",
                          bench.now_ms);
        bench.run(10);  // 100 ms
        CHECK(bench.hub.session.outstanding_count() > 0);
        bench.run(60);  // 600 ms more
        CHECK(bench.hub.session.outstanding_count() == 0);
        CHECK(bench.hub.responses.size() == 2);
    }
}

TEST_CASE("camera mock: fault console command (#122 stage 2)")
{
    camera_mock::Faults f;
    std::string err;
    CHECK(camera_mock::apply_fault_command(f, {"drop", "10"}, err));
    CHECK(f.drop_percent == 10);
    CHECK(camera_mock::apply_fault_command(f, {"battery", "15"}, err));
    CHECK(f.battery_percent == 15);
    CHECK_FALSE(camera_mock::apply_fault_command(f, {"drop", "101"}, err));
    CHECK(f.drop_percent == 10);
    CHECK_FALSE(camera_mock::apply_fault_command(f, {"mute", "x"}, err));
    CHECK(camera_mock::apply_fault_command(f, {"mute", "on"}, err));
    CHECK(f.mute);
    CHECK(camera_mock::apply_fault_command(f, {"off"}, err));
    CHECK_FALSE(f.any());
}

TEST_CASE("camera mock: thumbnails and photo download on the bulk channel "
          "(#122)")
{
    Bench bench;
    bench.discover_and_login();
    REQUIRE(bench.hub.responses.size() == 1);
    camera::artemis::MediaList list;
    REQUIRE(camera::artemis::parse_media_list(
        decrypt(fixtures::kMediaListPage0Response), list));
    REQUIRE(list.files.size() >= 3);
    std::uint32_t seq = 2;

    SUBCASE("three thumbnails in a row, as the hub's scan asks for them")
    {
        for (int i = 0; i < 3; i++) {
            const camera::artemis::MediaItem &item = list.files[i];
            bench.hub.command(seq++,
                              camera::artemis::build_thumbnail_request(
                                  item, fixtures::kCapturedToken),
                              bench.now_ms);
            bench.run(30);
            REQUIRE(bench.hub.records.size() ==
                    static_cast<std::size_t>(i + 1));
            const auto &record = bench.hub.records.back();
            CHECK(record.subtype == 0x04);
            CHECK(record.header.dir_num == item.dir_num);
            CHECK(record.header.media_num == item.num);
            CHECK(record.header.media_id == item.id);
            REQUIRE(record.file.size() >= 3000);
            CHECK(record.file[0] == 0xff);
            CHECK(record.file[1] == 0xd8);
            CHECK(record.file.back() == 0xd9);
        }
        CHECK(bench.hub.responses.size() == 4);
        CHECK(bench.hub.responses.back().find("\"cmdId\":772") !=
              std::string::npos);
        CHECK(bench.camera.stats().thumbnails == 3);
        CHECK(bench.camera.stats().bulk_retransmissions == 0);
    }

    SUBCASE("a photo (1285) on subtype 0x03")
    {
        const camera::artemis::MediaItem &item = list.files[0];
        bench.hub.command(seq++,
                          camera::artemis::build_download_request(
                              item, fixtures::kCapturedToken),
                          bench.now_ms);
        bench.run(40);
        REQUIRE(bench.hub.records.size() == 1);
        CHECK(bench.hub.records[0].subtype == 0x03);
        CHECK(bench.hub.records[0].header.media_num == item.num);
        CHECK(bench.hub.records[0].file.size() >= 40000);
        CHECK(bench.camera.stats().downloads == 1);
    }

    SUBCASE("unacknowledged frames are sent again; drops are recovered")
    {
        Faults lossy;
        lossy.drop_percent = 30;
        bench.camera.set_faults(lossy);
        // A photo: about 40 frames, so some are lost.
        bench.hub.command(seq++,
                          camera::artemis::build_download_request(
                              list.files[1], fixtures::kCapturedToken),
                          bench.now_ms);
        bench.run(400);
        REQUIRE(bench.hub.records.size() == 1);
        CHECK(bench.hub.records[0].header.media_num == list.files[1].num);
        CHECK(bench.camera.stats().dropped > 0);
        CHECK(bench.camera.stats().bulk_retransmissions > 0);
        CHECK(bench.camera.outstanding() == 0);
    }

    SUBCASE("a file that is not on the card: answer, no data")
    {
        bench.hub.command(seq++,
                          camera::artemis::build_thumbnail_request(
                              1, 999, 1, fixtures::kCapturedToken),
                          bench.now_ms);
        bench.run(20);
        CHECK(bench.hub.responses.size() == 2);
        CHECK(bench.hub.records.empty());
        CHECK(bench.hub.bulk_frames == 0);
    }
}

TEST_CASE("camera mock: identity (#122 stage 2)")
{
    Identity id;
    CHECK(mac_text(id) == "C6:1E:0D:E0:0C:FB");
    CHECK(ble_name(id) == std::string(fixtures::kBleName));
    CHECK(std::equal(id.dev_id, id.dev_id + 6, fixtures::kDevId));
    REQUIRE(parse_mac("c6-1e-0d-e0-0c-42", id));
    CHECK(mac_text(id) == "C6:1E:0D:E0:0C:42");
    CHECK(ble_name(id) == "TC100_0C42");
    CHECK_FALSE(parse_mac("C7:1E:0D:E0:0C:42", id));  // multicast
    CHECK_FALSE(parse_mac("C6:1E:0D:E0:0C", id));
    CHECK_FALSE(parse_mac("C6:1E:0D:E0:0C:4G", id));
    CHECK(mac_text(id) == "C6:1E:0D:E0:0C:42");

    // The hub reads the new MAC from 512 and from the media header.
    Bench bench;
    bench.camera.set_identity(id);
    bench.discover_and_login();
    bench.hub.command(2, "{\"cmdId\":512,\"token\":37817828}", bench.now_ms);
    bench.run();
    REQUIRE(bench.hub.responses.size() == 2);
    camera::artemis::DeviceInfo info;
    REQUIRE(camera::artemis::parse_device_info(bench.hub.responses[1], info));
    CHECK(info.dev_id == "C6:1E:0D:E0:0C:42");
    CHECK(info.bat_percent == 100);
}

TEST_CASE("camera mock: a bulk record stops mid-transfer (#122 stage 2)")
{
    Faults faults;
    std::string error;
    REQUIRE(apply_fault_command(faults, {"abort", "2"}, error));
    CHECK(faults.abort_after_frames == 2);
    CHECK(describe(faults) == "bulk records stop after 2 frames");
    CHECK_FALSE(apply_fault_command(faults, {"abort", "0"}, error));
    REQUIRE(apply_fault_command(faults, {"abort", "off"}, error));
    CHECK_FALSE(faults.any());

    Bench bench;
    bench.discover_and_login();
    camera::artemis::MediaList list;
    REQUIRE(camera::artemis::parse_media_list(
        decrypt(fixtures::kMediaListPage0Response), list));
    faults.abort_after_frames = 2;
    bench.camera.set_faults(faults);
    bench.hub.command(2,
                      camera::artemis::build_thumbnail_request(
                          list.files[0], fixtures::kCapturedToken),
                      bench.now_ms);
    bench.run(50);
    // The answer and two frames arrive; the record never completes, and
    // the hub has to give up on it (its stall check).
    CHECK(bench.hub.responses.size() == 2);
    CHECK(bench.hub.bulk_frames == 2);
    CHECK(bench.hub.records.empty());
    CHECK(bench.hub.thumbnail_bulk.header_ready());
    CHECK(bench.camera.stats().aborted_records == 1);
    CHECK(bench.camera.outstanding() == 0);

    // Without the fault the next request works again.
    bench.camera.set_faults(Faults());
    bench.hub.thumbnail_bulk.reset();
    bench.hub.command(3,
                      camera::artemis::build_thumbnail_request(
                          list.files[0], fixtures::kCapturedToken),
                      bench.now_ms);
    bench.run(30);
    CHECK(bench.hub.records.size() == 1);
}

TEST_CASE("camera mock: size of the media list (#122)")
{
    const camera::artemis::Aes128 cipher = camera::artemis::command_cipher();

    auto page = [&](std::size_t count, int page_no) {
        camera::artemis::MediaList list;
        REQUIRE(camera::artemis::parse_media_list(
            media_list_page(cipher, count, page_no, 45), list));
        return list;
    };

    SUBCASE("the captured size gives the captured entries")
    {
        camera::artemis::MediaList captured;
        REQUIRE(camera::artemis::parse_media_list(
            decrypt(fixtures::kMediaListPage1Response), captured));
        const auto list = page(90, 1);
        REQUIRE(list.files.size() == captured.files.size());
        for (std::size_t i = 0; i < list.files.size(); i++) {
            CHECK(list.files[i].id == captured.files[i].id);
            CHECK(list.files[i].num == captured.files[i].num);
        }
    }

    SUBCASE("fewer entries cut the list")
    {
        CHECK(page(50, 0).files.size() == 45);
        CHECK(page(50, 1).files.size() == 5);
        CHECK(page(50, 2).files.empty());
        CHECK(page(0, 0).files.empty());
    }

    SUBCASE("more entries go on with invented ones")
    {
        std::set<std::int64_t> ids;
        std::set<std::string> keys;
        std::size_t total = 0;
        for (int p = 0; p < 20; p++) {
            const auto list = page(700, p);
            for (const auto &item : list.files) {
                ids.insert(item.id);
                keys.insert(std::to_string(item.file_type) + "/" +
                            std::to_string(item.dir_num) + "/" +
                            std::to_string(item.num));
                CHECK(item.id != 0);
                CHECK(item.num > 0);
            }
            total += list.files.size();
        }
        CHECK(total == 700);
        CHECK(ids.size() == 700);
        CHECK(keys.size() == 700);
    }

    SUBCASE("a thumbnail request finds an invented entry, not one past the "
            "end")
    {
        const auto list = page(100, 2);
        REQUIRE(list.files.size() == 10);
        for (std::size_t i = 0; i < list.files.size(); i++) {
            const auto &item = list.files[i];
            CHECK(captured_media_id(cipher, item.file_type, item.dir_num,
                                    item.num, 100) == item.id);
            // Entries 90-94 stay in a list of 95, entries 95-99 do not.
            CHECK(captured_media_id(cipher, item.file_type, item.dir_num,
                                    item.num, 95) == (i < 5 ? item.id : 0));
        }
    }
}

TEST_CASE("camera mock: video playback (769/770, #122 stage 3)")
{
    Bench bench;
    bench.discover_and_login();
    REQUIRE(bench.hub.responses.size() == 1);
    camera::artemis::MediaList list;
    REQUIRE(camera::artemis::parse_media_list(
        decrypt(fixtures::kMediaListPage0Response), list));
    const camera::artemis::MediaItem *video = nullptr;
    const camera::artemis::MediaItem *photo = nullptr;
    for (const auto &item : list.files) {
        if (item.file_type != 0 && item.duration_ms > 0 && video == nullptr) {
            video = &item;
        }
        if (item.file_type == 0 && photo == nullptr) {
            photo = &item;
        }
    }
    REQUIRE(video != nullptr);
    REQUIRE(photo != nullptr);
    const std::string t =
        ",\"token\":" + std::to_string(fixtures::kCapturedToken) + "}";
    const auto play = [&](const camera::artemis::MediaItem &item,
                          std::uint32_t seq, int session_no) {
        bench.hub.command(
            seq,
            "{\"cmdId\":769,\"fileType\":" + std::to_string(item.file_type) +
                ",\"dirNum\":" + std::to_string(item.dir_num) +
                ",\"mediaNum\":" + std::to_string(item.num) +
                ",\"sessionNo\":" + std::to_string(session_no) + t,
            bench.now_ms);
    };

    SUBCASE("the stream decodes with the hub's own playback code")
    {
        play(*video, 2, 4711);
        bench.run(40);
        REQUIRE(bench.hub.responses.size() == 2);
        std::int64_t total = 0;
        std::int64_t start = -1;
        REQUIRE(find_int(bench.hub.responses.back(), "totalFrame", total));
        REQUIRE(find_int(bench.hub.responses.back(), "startPbRet", start));
        CHECK(start == 0);
        CHECK(total == static_cast<std::int64_t>(video->duration_ms / 40));

        // Everything the camera sends comes through the window; run on.
        for (int i = 0; i < 400 && bench.camera.stats().playbacks == 1; i++) {
            bench.run(20);
        }
        camera::artemis::PlaybackStream stream(256, 1 << 20);
        std::size_t video_frames = 0;
        std::size_t audio_frames = 0;
        bool first_is_key = false;
        const camera::artemis::Aes128 cipher =
            camera::artemis::command_cipher();
        const camera::artemis::PlaybackStream::RecordSink sink =
            [&](std::uint32_t version, std::uint32_t, std::uint8_t *payload,
                std::size_t size) {
                CHECK(version == 4);
                camera::artemis::PlaybackFrame frame;
                REQUIRE(camera::artemis::parse_playback_record(payload, size,
                                                               frame));
                CHECK(frame.session_no == 4711);
                camera::artemis::decrypt_playback_data(cipher, frame.data,
                                                       frame.size);
                if (frame.kind == camera::artemis::PlaybackKind::video) {
                    CHECK(frame.width == 1920);
                    CHECK(frame.height == 1080);
                    CHECK(frame.data[0] == 0);
                    CHECK(frame.data[3] == 1);
                    if (video_frames == 0) {
                        first_is_key = frame.data[4] == 0x67;
                    }
                    video_frames++;
                } else {
                    CHECK(frame.kind == camera::artemis::PlaybackKind::audio);
                    CHECK(frame.data[0] == 0xff);
                    CHECK((frame.data[1] & 0xf0) == 0xf0);
                    audio_frames++;
                }
                return true;
            };
        for (const auto &frame : bench.hub.playback_frames) {
            bool ack = false;
            CHECK(stream.feed(frame.seq, frame.payload.data(),
                              frame.payload.size(), sink, ack) ==
                  camera::artemis::PlaybackStream::Status::ok);
        }
        CHECK(first_is_key);
        CHECK(video_frames == static_cast<std::size_t>(total));
        CHECK(audio_frames == (video_frames + 1) / 2);
        CHECK(bench.camera.stats().playback_records ==
              video_frames + audio_frames);
    }

    SUBCASE("a photo, a missing file and a deleted video are refused")
    {
        play(*photo, 2, 1);
        bench.run(20);
        play(*video, 3, 1);
        bench.run(5);
        bench.hub.command(
            4,
            "{\"cmdId\":773,\"fileType\":" + std::to_string(video->file_type) +
                ",\"dirNum\":" + std::to_string(video->dir_num) +
                ",\"mediaNum\":" + std::to_string(video->num) + t,
            bench.now_ms);
        bench.run(20);
        bench.hub.playback_frames.clear();
        play(*video, 5, 2);
        bench.run(20);
        REQUIRE(bench.hub.responses.size() >= 5);
        CHECK(bench.hub.responses[1].find("\"startPbRet\":-3") !=
              std::string::npos);
        CHECK(bench.hub.responses.back().find("\"startPbRet\":-3") !=
              std::string::npos);
        CHECK(bench.camera.stats().playbacks == 1);
    }

    SUBCASE("770 stops the stream")
    {
        bench.hub.ack_bulk = true;
        play(*video, 2, 7);
        bench.run(5);
        const std::size_t before = bench.hub.playback_frames.size();
        REQUIRE(before > 0);
        bench.hub.command(3, "{\"cmdId\":770" + t, bench.now_ms);
        bench.run(5);
        const std::size_t after = bench.hub.playback_frames.size();
        bench.run(40);
        CHECK(bench.hub.playback_frames.size() == after);
        CHECK(bench.camera.stats().playback_records < 100);
    }

    SUBCASE("the abort fault ends the stream early")
    {
        Faults faults;
        faults.abort_after_frames = 10;
        bench.camera.set_faults(faults);
        play(*video, 2, 9);
        bench.run(60);
        CHECK(bench.hub.playback_frames.size() == 10);
        CHECK(bench.camera.stats().aborted_records == 1);
    }
}
