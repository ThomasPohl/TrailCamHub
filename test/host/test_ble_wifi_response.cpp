#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ble_wifi_response.hpp"

using camera::ble::WifiResponseAssembler;
using Event = WifiResponseAssembler::Event;

namespace
{

// Same structure as the camera's response; the password is a placeholder.
const std::string kJson = "{\"ret\":0,\"ssid\":\"TrailCam_897B\","
                          "\"bssid\":\"1C:4E:A2:74:89:7B\",\"pwd\":\"secret\"}";

std::vector<std::uint8_t> frame(const std::string &json,
                                std::uint16_t message_type = 0x0001)
{
    const auto size = static_cast<std::uint32_t>(json.size());
    std::vector<std::uint8_t> bytes = {
        0x13,
        0x57,
        static_cast<std::uint8_t>(message_type),
        static_cast<std::uint8_t>(message_type >> 8),
        static_cast<std::uint8_t>(size),
        static_cast<std::uint8_t>(size >> 8),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 24),
    };
    bytes.insert(bytes.end(), json.begin(), json.end());
    return bytes;
}

// Feeds `bytes` and returns the events other than Event::none.
std::vector<Event> feed(WifiResponseAssembler &assembler,
                        const std::vector<std::uint8_t> &bytes)
{
    std::vector<Event> events;
    for (std::uint8_t byte : bytes) {
        const Event event = assembler.push(byte);
        if (event != Event::none) {
            events.push_back(event);
        }
    }
    return events;
}

}  // namespace

TEST_CASE("reassembles a frame split into 20-byte notifications")
{
    WifiResponseAssembler assembler;
    const auto bytes = frame(kJson);
    std::vector<Event> events;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 20) {
        const std::size_t end = std::min(bytes.size(), offset + 20);
        const auto chunk =
            feed(assembler, std::vector<std::uint8_t>(bytes.begin() + offset,
                                                      bytes.begin() + end));
        events.insert(events.end(), chunk.begin(), chunk.end());
    }
    REQUIRE(events.size() == 2);
    CHECK(events[0] == Event::header_received);
    CHECK(events[1] == Event::frame_complete);
    CHECK(assembler.message_type() == 0x0001);
    REQUIRE(assembler.json_size() == kJson.size());
    CHECK(std::string(reinterpret_cast<const char *>(assembler.json()),
                      assembler.json_size()) == kJson);
}

TEST_CASE("skips bytes before the magic and resynchronizes")
{
    WifiResponseAssembler assembler;
    auto bytes = frame(kJson);
    bytes.insert(bytes.begin(), {0x00, 0x13, 0x00, 0x13});
    const auto events = feed(assembler, bytes);
    REQUIRE(events.size() == 2);
    CHECK(events[1] == Event::frame_complete);
}

TEST_CASE("rejects invalid JSON lengths")
{
    WifiResponseAssembler assembler;
    CHECK(feed(assembler, frame(""))[0] == Event::invalid_length);

    std::vector<std::uint8_t> too_long = {0x13, 0x57, 0x01, 0x00,
                                          0x01, 0x02, 0x00, 0x00};
    const auto events = feed(assembler, too_long);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == Event::invalid_length);
}

TEST_CASE("starts a new frame after a completed one")
{
    WifiResponseAssembler assembler;
    auto bytes = frame(kJson);
    const auto second = frame("{\"ret\":1}", 0x0002);
    bytes.insert(bytes.end(), second.begin(), second.end());
    const auto events = feed(assembler, bytes);
    REQUIRE(events.size() == 4);
    CHECK(events[3] == Event::frame_complete);
    CHECK(assembler.message_type() == 0x0002);
    CHECK(std::string(reinterpret_cast<const char *>(assembler.json()),
                      assembler.json_size()) == "{\"ret\":1}");
}
