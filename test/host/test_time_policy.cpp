#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "time_policy.hpp"

using timepolicy::may_set;
using timepolicy::Source;

TEST_CASE("time policy: a better source replaces a worse one")
{
    // The clock holds a time.
    CHECK(may_set(Source::camera, Source::browser, true));
    CHECK(may_set(Source::browser, Source::sntp, true));
    CHECK(may_set(Source::backend, Source::sntp, true));
    CHECK(may_set(Source::sntp, Source::sntp, true));
    CHECK(may_set(Source::browser, Source::browser, true));
    // A worse one does not replace it.
    CHECK_FALSE(may_set(Source::sntp, Source::browser, true));
    CHECK_FALSE(may_set(Source::sntp, Source::camera, true));
    CHECK_FALSE(may_set(Source::backend, Source::camera, true));
    CHECK_FALSE(may_set(Source::browser, Source::camera, true));
}

TEST_CASE("time policy: any source sets a clock that holds no time")
{
    CHECK(may_set(Source::none, Source::camera, false));
    CHECK(may_set(Source::none, Source::browser, false));
    // Even a worse source than the one that set it, if the clock was lost.
    CHECK(may_set(Source::sntp, Source::camera, false));
}

TEST_CASE("time policy: only browser or better is given to the camera")
{
    CHECK_FALSE(timepolicy::trusted(Source::none));
    CHECK_FALSE(timepolicy::trusted(Source::camera));
    CHECK(timepolicy::trusted(Source::browser));
    CHECK(timepolicy::trusted(Source::backend));
    CHECK(timepolicy::trusted(Source::sntp));
    CHECK(std::strcmp(timepolicy::source_name(Source::none), "none") == 0);
    CHECK(std::strcmp(timepolicy::source_name(Source::camera), "camera") == 0);
    CHECK(std::strcmp(timepolicy::source_name(Source::browser), "browser") ==
          0);
    CHECK(std::strcmp(timepolicy::source_name(Source::backend), "backend") ==
          0);
    CHECK(std::strcmp(timepolicy::source_name(Source::sntp), "sntp") == 0);
}

TEST_CASE("time policy: the build date of the firmware")
{
    // 2026-10-31 21:21:50 UTC
    CHECK(timepolicy::parse_build_time("Oct 31 2026", "21:21:50") ==
          1793481710);
    // The compiler pads a single digit day with a space.
    CHECK(timepolicy::parse_build_time("Jan  5 2027", "00:00:00") ==
          1799107200);
    CHECK(timepolicy::parse_build_time("Foo 31 2026", "21:21:50") == 0);
    CHECK(timepolicy::parse_build_time("Oct 31 20x6", "21:21:50") == 0);
    CHECK(timepolicy::parse_build_time("Oct 31 2026", "21-21-50") == 0);
    CHECK(timepolicy::parse_build_time("Oct", "21:21:50") == 0);
    CHECK(timepolicy::parse_build_time(nullptr, nullptr) == 0);
}

TEST_CASE("time policy: plausible times")
{
    const std::int64_t build = 1793481710;  // 2026-10-31 21:21:50
    CHECK(timepolicy::plausible(1793484891, build));
    CHECK(timepolicy::plausible(build, build));
    // A day of slack for the time zone of the build machine.
    CHECK(timepolicy::plausible(build - 86400, build));
    CHECK_FALSE(timepolicy::plausible(build - 86401, build));
    // The stale clock that firmware used to set on the camera.
    CHECK_FALSE(timepolicy::plausible(1766505566, build));
    CHECK_FALSE(timepolicy::plausible(0, build));
    CHECK_FALSE(timepolicy::plausible(157, build));
    CHECK_FALSE(timepolicy::plausible(4102444800, build));
    // Ten years after the build at most: 2038 is too far, 2030 is not.
    CHECK(timepolicy::plausible(1900000000, build));        // 2030
    CHECK_FALSE(timepolicy::plausible(2147483647, build));  // 2038-01-19
    CHECK_FALSE(timepolicy::plausible(4500000000, build));
    // Without a build date, the start of 2024 is the limit.
    CHECK(timepolicy::plausible(1766505566, 0));
    CHECK_FALSE(timepolicy::plausible(1704067199, 0));
    CHECK(timepolicy::plausible(1704067200, 0));
    CHECK(timepolicy::plausible(2147483647, 0));
    CHECK_FALSE(timepolicy::plausible(4102444800, 0));
}
