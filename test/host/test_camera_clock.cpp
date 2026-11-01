#include <doctest/doctest.h>

#include <stdlib.h>
#include <time.h>

#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"
#include "artemis_login.hpp"
#include "artemis_message.hpp"
#include "camera_clock.hpp"

using camera::camera_wall_clock;

namespace
{
// Sets the time zone of the C library, as the firmware does at boot.
void use_time_zone(const char *rule)
{
    setenv("TZ", rule, 1);
    tzset();
}
}  // namespace

TEST_CASE("camera_wall_clock: Central European Time and Summer Time")
{
    use_time_zone("CET-1CEST,M3.5.0,M10.5.0/3");
    // 2026: summer time from Sunday 29 March 01:00 UTC to Sunday 25 October
    // 01:00 UTC.
    CHECK(camera_wall_clock(1774745999) == 1774745999 + 3600);
    CHECK(camera_wall_clock(1774746000) == 1774746000 + 7200);
    CHECK(camera_wall_clock(1792889999) == 1792889999 + 7200);
    CHECK(camera_wall_clock(1792890000) == 1792890000 + 3600);
    // Winter, summer and the time of the first tests of this firmware.
    CHECK(camera_wall_clock(1768478400) == 1768478400 + 3600);
    CHECK(camera_wall_clock(1784116800) == 1784116800 + 7200);
    CHECK(camera_wall_clock(1793479646) == 1793479646 + 3600);
    // 2025: 30 March and 26 October.
    CHECK(camera_wall_clock(1743296400) == 1743296400 + 7200);
    CHECK(camera_wall_clock(1761440399) == 1761440399 + 7200);
}

TEST_CASE("camera_wall_clock: follows the rule of any zone")
{
    use_time_zone("UTC0");
    CHECK(camera_wall_clock(1784116800) == 1784116800);
    use_time_zone("IST-5:30");  // India, no summer time
    CHECK(camera_wall_clock(1784116800) == 1784116800 + 19800);
    use_time_zone("EST5EDT,M3.2.0,M11.1.0");  // US Eastern
    CHECK(camera_wall_clock(1784116800) == 1784116800 - 4 * 3600);
    CHECK(camera_wall_clock(1768478400) == 1768478400 - 5 * 3600);
    use_time_zone("CET-1CEST,M3.5.0,M10.5.0/3");
}

TEST_CASE("login JSON: with the camera time, and without one")
{
    CHECK(camera::artemis::build_login_json(1766505566) ==
          "{\"cmdId\":0,\"usrName\":\"admin\",\"password\":\"admin\","
          "\"needVideo\":0,\"needAudio\":0,\"utcTime\":1766505566,"
          "\"supportHeartBeat\":true}");
    // No usable time: the field is left out, so that the camera keeps its own.
    const std::string without = "{\"cmdId\":0,\"usrName\":\"admin\","
                                "\"password\":\"admin\",\"needVideo\":0,"
                                "\"needAudio\":0,\"supportHeartBeat\":true}";
    CHECK(camera::artemis::build_login_json(0) == without);
    CHECK(camera::artemis::build_login_json(-5) == without);
}

TEST_CASE("login request: same login as the block that was hard-coded before")
{
    // The firmware used to send this captured login, which sets the camera
    // clock to 2025-12-23 15:59:26 at every session. After the closing NUL of
    // its JSON text it has leftover bytes of the app's buffer, which the
    // camera ignores; ours pads with zeros, as all other commands do. The
    // JSON text is the same.
    static const std::uint8_t kOldHello[] = {
        0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0xad, 0x00, 0x00, 0x00, 0x4a, 0x38, 0x57, 0x57,
        0x75, 0x51, 0x44, 0x50, 0x6d, 0x59, 0x53, 0x4c, 0x66, 0x75, 0x2f, 0x67,
        0x58, 0x41, 0x47, 0x2b, 0x55, 0x71, 0x62, 0x42, 0x79, 0x35, 0x35, 0x4b,
        0x50, 0x32, 0x69, 0x45, 0x32, 0x35, 0x51, 0x50, 0x4e, 0x6f, 0x66, 0x7a,
        0x6e, 0x30, 0x34, 0x30, 0x2b, 0x4e, 0x49, 0x39, 0x67, 0x37, 0x7a, 0x65,
        0x58, 0x4c, 0x6b, 0x49, 0x70, 0x58, 0x70, 0x43, 0x30, 0x37, 0x53, 0x58,
        0x76, 0x6f, 0x73, 0x72, 0x57, 0x73, 0x63, 0x31, 0x6d, 0x38, 0x6d, 0x78,
        0x6e, 0x71, 0x36, 0x68, 0x4d, 0x69, 0x4b, 0x77, 0x65, 0x50, 0x62, 0x4b,
        0x4a, 0x55, 0x77, 0x76, 0x53, 0x76, 0x71, 0x5a, 0x62, 0x36, 0x73, 0x30,
        0x73, 0x6c, 0x31, 0x73, 0x66, 0x7a, 0x68, 0x33, 0x53, 0x35, 0x30, 0x70,
        0x70, 0x30, 0x74, 0x75, 0x32, 0x4b, 0x65, 0x77, 0x69, 0x30, 0x50, 0x69,
        0x44, 0x63, 0x76, 0x58, 0x71, 0x58, 0x4d, 0x32, 0x68, 0x50, 0x6c, 0x4e,
        0x6c, 0x68, 0x47, 0x53, 0x69, 0x33, 0x46, 0x55, 0x41, 0x76, 0x2b, 0x50,
        0x64, 0x79, 0x35, 0x68, 0x2f, 0x72, 0x78, 0x38, 0x2b, 0x47, 0x74, 0x37,
        0x37, 0x54, 0x68, 0x45, 0x2b, 0x72, 0x64, 0x31, 0x44, 0x6d, 0x45, 0x3d,
        0x00,
    };
    static const std::uint8_t kKey[16] = {'x', 's', '3', '8', 'n', 'u',
                                          'l', '7', 'c', 'q', 'f', '7',
                                          'm', '1', 'v', 'a'};
    const camera::artemis::Aes128 cipher(kKey);
    const std::string expected = camera::artemis::build_login_json(1766505566);

    camera::artemis::Message old_message;
    REQUIRE(camera::artemis::parse(cipher, kOldHello, sizeof(kOldHello),
                                   old_message));
    CHECK(old_message.json == expected);

    const std::vector<std::uint8_t> built =
        camera::artemis::build_request(cipher, 1, expected);
    REQUIRE(!built.empty());
    camera::artemis::Message new_message;
    REQUIRE(camera::artemis::parse(cipher, built.data(), built.size(),
                                   new_message));
    CHECK(new_message.json == expected);
    // Same record header (signature, version, type) as the old block.
    CHECK(std::vector<std::uint8_t>(built.begin(), built.begin() + 16) ==
          std::vector<std::uint8_t>(kOldHello, kOldHello + 16));
}
