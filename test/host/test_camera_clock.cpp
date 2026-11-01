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

TEST_CASE("camera_wall_clock_to_utc: the inverse of camera_wall_clock")
{
    use_time_zone("CET-1CEST,M3.5.0,M10.5.0/3");
    using camera::camera_wall_clock_to_utc;
    // What the camera shows is local time: 1 h or 2 h ahead of UTC.
    CHECK(camera_wall_clock_to_utc(1793484891) == 1793484891 - 3600);
    CHECK(camera_wall_clock_to_utc(1784116800 + 7200) == 1784116800);
    // Round trips, in winter, in summer, and either side of the changes.
    for (std::int64_t utc :
         {1768478400LL, 1784116800LL, 1793481710LL, 1774745000LL, 1774747000LL,
          1792880000LL, 1792893000LL, 1743296000LL, 1761441000LL}) {
        CHECK(camera_wall_clock_to_utc(camera_wall_clock(utc)) == utc);
    }
    // The hour that is shown twice when summer time ends (02:00 to 03:00 on
    // 25 October 2026): either of the two instants comes back.
    const std::int64_t first = 1792889000;   // 02:43 CEST
    const std::int64_t second = 1792892600;  // 02:43 CET
    CHECK(camera_wall_clock(first) == camera_wall_clock(second));
    const std::int64_t back =
        camera_wall_clock_to_utc(camera_wall_clock(first));
    CHECK((back == first || back == second));
    use_time_zone("EST5EDT,M3.2.0,M11.1.0");
    CHECK(camera_wall_clock_to_utc(camera_wall_clock(1784116800)) ==
          1784116800);
    use_time_zone("CET-1CEST,M3.5.0,M10.5.0/3");
}

TEST_CASE("media_file_time: the FAT file shows the camera's wall clock")
{
    use_time_zone("CET-1CEST,M3.5.0,M10.5.0/3");
    using camera::media_file_time;
    // An unset camera clock keeps the default file time.
    CHECK(media_file_time(0) == 0);
    CHECK(media_file_time(-5) == 0);
    // A photo taken at 14:00 on the camera (wall clock read as UTC): the file
    // time is the real instant, 13:00 UTC in winter and 12:00 UTC in summer.
    CHECK(media_file_time(1768485600) == 1768485600 - 3600);  // 15.01.2026
    CHECK(media_file_time(1784124000) == 1784124000 - 7200);  // 15.07.2026
    // The FAT driver stores localtime_r() of the file time: that local
    // calendar time must be the wall clock again.
    for (std::int64_t wall : {1768485600LL, 1784124000LL, 1793484891LL,
                              1766505692LL, 1774753200LL}) {
        const std::int64_t file_time = media_file_time(wall);
        const time_t instant = static_cast<time_t>(file_time);
        struct tm local = {};
        REQUIRE(localtime_r(&instant, &local) != nullptr);
        struct tm wall_tm = {};
        const time_t wall_instant = static_cast<time_t>(wall);
        REQUIRE(gmtime_r(&wall_instant, &wall_tm) != nullptr);
        CHECK(local.tm_year == wall_tm.tm_year);
        CHECK(local.tm_mon == wall_tm.tm_mon);
        CHECK(local.tm_mday == wall_tm.tm_mday);
        CHECK(local.tm_hour == wall_tm.tm_hour);
        CHECK(local.tm_min == wall_tm.tm_min);
        CHECK(local.tm_sec == wall_tm.tm_sec);
    }
    // In UTC the wall clock is the file time.
    use_time_zone("UTC0");
    CHECK(media_file_time(1784124000) == 1784124000);
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
