#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"
#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "artemis_message.hpp"
#include "camera_udp_packets.hpp"
#include "pppp_session.hpp"

using namespace camera;
using namespace camera::artemis;
using State = LoginSession::State;

namespace
{

class FakeTransport : public pppp::Transport
{
  public:
    bool send(const std::uint8_t *data, std::size_t size,
              const char *context) override
    {
        sent.emplace_back(data, data + size);
        contexts.emplace_back(context);
        return true;
    }
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::string> contexts;
};

void append_u32_le(std::vector<std::uint8_t> &v, std::uint32_t n)
{
    v.push_back(static_cast<std::uint8_t>(n & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 24) & 0xff));
}

// A full ARTEMIS response message carrying the given JSON, encrypted.
std::vector<std::uint8_t> response_message(const Aes128 &cipher,
                                           std::uint32_t app_sequence,
                                           const std::string &json)
{
    const std::string body = encode(cipher, json);
    std::vector<std::uint8_t> v = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    append_u32_le(v, kMsgResponse);
    append_u32_le(v, app_sequence);
    append_u32_le(v, static_cast<std::uint32_t>(body.size()));
    v.insert(v.end(), body.begin(), body.end());
    v.push_back(0);
    return v;
}

// Wrap an ARTEMIS message as an RUDP DATA packet from the camera.
std::vector<std::uint8_t> camera_data(std::uint16_t index,
                                      const std::vector<std::uint8_t> &artemis)
{
    ArtemisDataRequest dr;
    REQUIRE(dr.build(index, artemis.data(), artemis.size()));
    return std::vector<std::uint8_t>(dr.data(), dr.data() + dr.size());
}

// Decode an ARTEMIS request that was sent through the transport (an RUDP DATA
// packet).
Message parse_sent(const Aes128 &cipher, const std::vector<std::uint8_t> &pkt)
{
    ArtemisRudpPacket rudp;
    REQUIRE(rudp.parse(pkt.data(), pkt.size()));
    REQUIRE(rudp.is_data());
    const std::uint8_t *payload = rudp.data() + ArtemisRudpPacket::kHeaderSize;
    const std::size_t size = rudp.size() - ArtemisRudpPacket::kHeaderSize;
    Message msg;
    REQUIRE(parse(cipher, payload, size, msg));
    return msg;
}

}  // namespace

TEST_CASE("json_find_int reads flat integer fields")
{
    const std::string json = "{\"cmdId\":0,\"result\":0,\"token\":113034621}";
    std::int64_t v = -1;
    REQUIRE(json_find_int(json, "cmdId", v));
    CHECK(v == 0);
    REQUIRE(json_find_int(json, "token", v));
    CHECK(v == 113034621);
    CHECK_FALSE(json_find_int(json, "missing", v));
}

TEST_CASE("login succeeds and the token authenticates later commands")
{
    FakeTransport transport;
    pppp::Session session(transport);
    Aes128 cipher = command_cipher();
    LoginSession login(session, cipher);

    CHECK(login.state() == State::idle);

    REQUIRE(login.start(1766505566, 1000));
    CHECK(login.state() == State::logging_in);
    REQUIRE(transport.sent.size() == 1);

    // The sent login request is cmdId 0 with the credentials.
    Message request = parse_sent(cipher, transport.sent[0]);
    CHECK(request.msg_type == kMsgRequest);
    std::int64_t cmd = -1;
    REQUIRE(json_find_int(request.json, "cmdId", cmd));
    CHECK(cmd == 0);

    // Camera answers with the login response (DATA index 0).
    std::vector<std::uint8_t> resp = camera_data(
        0, response_message(cipher, 1,
                            "{\"cmdId\":0,\"result\":0,\"token\":113034621,"
                            "\"bat_percent\":80,\"errorMsg\":\"Success\"}"));
    login.on_packet(resp.data(), resp.size(), 2000);

    CHECK(login.logged_in());
    CHECK(login.token() == 113034621);
    // The camera DATA was acknowledged (an extra packet went out).
    CHECK(transport.sent.size() == 2);

    // A follow-up command carries the token.
    REQUIRE(login.send_command(512, 3000));
    REQUIRE(transport.sent.size() == 3);
    Message cmd_msg = parse_sent(cipher, transport.sent[2]);
    CHECK(cmd_msg.json == "{\"cmdId\":512,\"token\":113034621}");
}

TEST_CASE("login fails when the camera rejects it")
{
    FakeTransport transport;
    pppp::Session session(transport);
    Aes128 cipher = command_cipher();
    LoginSession login(session, cipher);

    REQUIRE(login.start(1766505566, 1000));
    std::vector<std::uint8_t> resp = camera_data(
        0,
        response_message(cipher, 1,
                         "{\"cmdId\":0,\"result\":1,\"errorMsg\":\"denied\"}"));
    login.on_packet(resp.data(), resp.size(), 2000);

    CHECK(login.state() == State::failed);
    CHECK_FALSE(login.logged_in());
    CHECK_FALSE(login.send_command(512, 3000));
}

TEST_CASE("a fragmented command response is reassembled")
{
    FakeTransport transport;
    pppp::Session session(transport);
    Aes128 cipher = command_cipher();
    LoginSession login(session, cipher);

    REQUIRE(login.start(1766505566, 1000));
    std::vector<std::uint8_t> login_resp = camera_data(
        0, response_message(cipher, 1,
                            "{\"cmdId\":0,\"result\":0,\"token\":113034621}"));
    login.on_packet(login_resp.data(), login_resp.size(), 2000);
    REQUIRE(login.logged_in());
    CHECK(login.last_response_cmd() == 0);

    REQUIRE(login.request_file_list(3000));

    // A long response that spans two packets.
    const std::string files =
        "{\"cmdId\":768,\"result\":0,\"files\":[{\"n\":\"IMG_0001.JPG\"},"
        "{\"n\":\"IMG_0002.JPG\"},{\"n\":\"IMG_0003.JPG\"}]}";
    const std::vector<std::uint8_t> full = response_message(cipher, 2, files);
    const std::size_t cut = full.size() / 2;

    // First half as a 0x42 fragment (index 1), the rest as DATA (index 2).
    std::vector<std::uint8_t> part0 = camera_data(
        1, std::vector<std::uint8_t>(full.begin(), full.begin() + cut));
    part0[1] = 0x42;
    std::vector<std::uint8_t> part1 = camera_data(
        2, std::vector<std::uint8_t>(full.begin() + cut, full.end()));

    login.on_packet(part0.data(), part0.size(), 3100);
    CHECK(login.last_response_cmd() == 0);  // still incomplete
    login.on_packet(part1.data(), part1.size(), 3200);

    CHECK(login.last_response_cmd() == 768);
    CHECK(login.last_response_json() == files);
}

TEST_CASE("a CLOSE from the camera is reported")
{
    FakeTransport transport;
    pppp::Session session(transport);
    Aes128 cipher = command_cipher();
    LoginSession login(session, cipher);
    REQUIRE(login.start(1766505566, 1000));

    const std::uint8_t close[4] = {0xf1, 0xf0, 0x00, 0x00};
    login.on_packet(close, sizeof(close), 2000);
    CHECK(login.closed());
}

TEST_CASE("json_set_int replaces or adds an integer field")
{
    const std::string json = "{\"devId\":\"X\",\"readAt\":5}";
    CHECK(json_set_int(json, "readAt", 9) == "{\"devId\":\"X\",\"readAt\":9}");
    CHECK(json_set_int(json, "lastSyncAt", -1) ==
          "{\"devId\":\"X\",\"readAt\":5,\"lastSyncAt\":-1}");
    CHECK(json_set_int("{\"a\":-3,\"b\":1}", "a", 12) == "{\"a\":12,\"b\":1}");
    CHECK(json_set_int("{}", "k", 1) == "{\"k\":1}");
    std::int64_t value = 0;
    REQUIRE(json_find_int(json_set_int(json, "lastSyncAt", 1730400000),
                          "lastSyncAt", value));
    CHECK(value == 1730400000);
}
