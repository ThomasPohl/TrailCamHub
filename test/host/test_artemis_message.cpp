#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"
#include "artemis_crypto.hpp"
#include "artemis_message.hpp"

using namespace camera::artemis;

namespace
{

void append_u32_le(std::vector<std::uint8_t> &v, std::uint32_t n)
{
    v.push_back(static_cast<std::uint8_t>(n & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 24) & 0xff));
}

// Assembles an ARTEMIS message around an already-Base64-encoded body.
std::vector<std::uint8_t> frame(std::uint32_t msg_type,
                                std::uint32_t app_sequence,
                                const std::string &base64_body)
{
    std::vector<std::uint8_t> v = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    append_u32_le(v, msg_type);
    append_u32_le(v, app_sequence);
    append_u32_le(v, static_cast<std::uint32_t>(base64_body.size()));
    v.insert(v.end(), base64_body.begin(), base64_body.end());
    v.push_back(0);  // NUL terminator
    return v;
}

}  // namespace

TEST_CASE("build_request() produces a parseable request")
{
    Aes128 cipher = command_cipher();
    const std::string json = "{\"cmdId\":512,\"token\":113034621}";

    std::vector<std::uint8_t> msg = build_request(cipher, 7, json);
    REQUIRE_FALSE(msg.empty());

    // Signature and little-endian header fields.
    CHECK(std::string(msg.begin(), msg.begin() + 7) == "ARTEMIS");
    CHECK(msg[7] == 0);

    Message parsed;
    REQUIRE(parse(cipher, msg.data(), msg.size(), parsed));
    CHECK(parsed.msg_type == kMsgRequest);
    CHECK(parsed.app_sequence == 7);
    CHECK(parsed.json == json);
}

TEST_CASE("parse() decodes a captured login response message")
{
    Aes128 cipher = command_cipher();
    // Real login-response body (MsgType 3) wrapped in an ARTEMIS header.
    const std::string body =
        "7sQ3+pH/khrx+xvDdflBd8zgJF1T4ImASXycUhSI4p9PW9TYvHzT6Tuh"
        "7qFyCvRTmd4sQESImycJCGKho43h1WULW93yE+lNbmV4tSFUgyA=";
    std::vector<std::uint8_t> msg = frame(kMsgResponse, 1, body);

    Message parsed;
    REQUIRE(parse(cipher, msg.data(), msg.size(), parsed));
    CHECK(parsed.msg_type == kMsgResponse);
    CHECK(parsed.app_sequence == 1);
    CHECK(parsed.json ==
          "{\"cmdId\":0,\"result\":0,\"token\":113034621,\"bat_percent\":80,"
          "\"errorMsg\":\"Success\"}");
}

TEST_CASE("parse() handles a PayloadLen that includes NUL and zero padding")
{
    Aes128 cipher = command_cipher();
    // The real camera's login response: a 108-char Base64 string, but
    // PayloadLen is 129 (0x81) because it also counts the NUL terminator and
    // trailing zero padding.
    const std::string b64 =
        "7sQ3+pH/khrx+xvDdflBd8zgJF1T4ImASXycUhSI4p9PW9TYvHzT6Tuh"
        "7qFyCvRTmd4sQESImycJCGKho43h1WULW93yE+lNbmV4tSFUgyA=";

    std::vector<std::uint8_t> msg = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    append_u32_le(msg, kMsgResponse);
    append_u32_le(msg, 1);
    append_u32_le(msg, static_cast<std::uint32_t>(b64.size()) + 21);
    msg.insert(msg.end(), b64.begin(), b64.end());
    msg.resize(msg.size() + 21, 0);  // NUL terminator + zero padding

    Message parsed;
    REQUIRE(parse(cipher, msg.data(), msg.size(), parsed));
    CHECK(parsed.json ==
          "{\"cmdId\":0,\"result\":0,\"token\":113034621,\"bat_percent\":80,"
          "\"errorMsg\":\"Success\"}");
}

TEST_CASE("parse() rejects malformed messages")
{
    Aes128 cipher = command_cipher();
    const std::string body = encode(cipher, "{\"cmdId\":525}");
    std::vector<std::uint8_t> good = frame(kMsgRequest, 1, body);
    Message parsed;

    SUBCASE("too short")
    {
        CHECK_FALSE(parse(cipher, good.data(), 10, parsed));
    }
    SUBCASE("wrong signature")
    {
        std::vector<std::uint8_t> bad = good;
        bad[0] = 'X';
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
    SUBCASE("payload length beyond the buffer")
    {
        std::vector<std::uint8_t> bad = good;
        bad[16] = 0xff;  // inflate PayloadLen (little-endian LSB)
        bad[17] = 0xff;
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
    SUBCASE("body not valid Base64 blocks")
    {
        std::vector<std::uint8_t> bad = frame(kMsgRequest, 1, "AAAA");
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
}

TEST_CASE("heartbeat: cmdId 525 in the app's own sequence range")
{
    const Aes128 cipher = command_cipher();
    // Captured from the TrailCam Go app: sequence 0x00010002, body
    // {"cmdId":525}.
    const std::vector<std::uint8_t> captured = {
        0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x4d, 0x7a, 0x6c, 0x42,
        0x33, 0x36, 0x58, 0x2f, 0x49, 0x56, 0x6f, 0x38, 0x5a, 0x7a, 0x49, 0x35,
        0x72, 0x47, 0x39, 0x6a, 0x31, 0x77, 0x3d, 0x3d, 0x00,
    };
    Message app;
    REQUIRE(parse(cipher, captured.data(), captured.size(), app));
    CHECK(app.json == kHeartbeatJson);

    Heartbeat heartbeat;
    CHECK(heartbeat.count() == 0);
    for (std::uint32_t n = 1; n <= 2; ++n) {
        const std::vector<std::uint8_t> built = heartbeat.next(cipher);
        CHECK(heartbeat.count() == n);
        Message ours;
        REQUIRE(parse(cipher, built.data(), built.size(), ours));
        CHECK(ours.msg_type == kMsgRequest);
        CHECK(ours.app_sequence == kHeartbeatSequenceBase + n);
        CHECK(ours.json == app.json);
        // Same header layout as the app's up to the sequence.
        CHECK(
            std::vector<std::uint8_t>(built.begin(), built.begin() + 12) ==
            std::vector<std::uint8_t>(captured.begin(), captured.begin() + 12));
    }
}

TEST_CASE("heartbeat counter wraps to 1")
{
    const Aes128 cipher = command_cipher();
    Heartbeat heartbeat;
    for (int i = 0; i < 0xffff; ++i) {
        heartbeat.next(cipher);
    }
    CHECK(heartbeat.count() == 0xffff);
    const std::vector<std::uint8_t> wrapped = heartbeat.next(cipher);
    CHECK(heartbeat.count() == 1);
    Message message;
    REQUIRE(parse(cipher, wrapped.data(), wrapped.size(), message));
    CHECK(message.app_sequence == kHeartbeatSequenceBase + 1);
}
