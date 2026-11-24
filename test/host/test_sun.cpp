#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "sun.hpp"

using namespace schedule;

namespace
{

// Local clock text of a UTC minute count shifted by an offset (minutes).
int local_of(double utc_minutes, int offset_min)
{
    return local_minutes(utc_minutes, offset_min);
}

}  // namespace

TEST_CASE("locations are parsed, rounded and formatted")
{
    Location location;
    std::string error;
    REQUIRE(parse_location("52.52,13.405", location, &error));
    CHECK(location.valid);
    CHECK(format_location(location) == "52.5,13.4");
    REQUIRE(parse_location("-33.87; 151.21", location, &error));
    CHECK(format_location(location) == "-33.9,151.2");
    REQUIRE(parse_location("48.1 11.6", location, &error));
    CHECK(format_location(location) == "48.1,11.6");
    // No "-0.0".
    REQUIRE(parse_location("0.01,-0.04", location, &error));
    CHECK(format_location(location) == "0.0,0.0");

    CHECK_FALSE(parse_location("", location, &error));
    CHECK_FALSE(parse_location("52.5", location, &error));
    CHECK_FALSE(parse_location("52.5,", location, &error));
    CHECK_FALSE(parse_location("52.5,13.4,1", location, &error));
    CHECK_FALSE(parse_location("north,east", location, &error));
    CHECK_FALSE(parse_location("91,0", location, &error));
    CHECK_FALSE(parse_location("0,181", location, &error));
    CHECK_FALSE(error.empty());
    CHECK(format_location(Location()).empty());
}

TEST_CASE("sunrise and sunset in Berlin match the reference within 3 minutes")
{
    double rise = 0;
    double set = 0;
    // Berlin, 52.52 N 13.405 E. Reference (timeanddate): 2026-06-21 sunrise
    // 04:43, sunset 21:33 CEST; 2026-12-21 sunrise 08:16, sunset 15:54 CET.
    REQUIRE(sun_utc_minutes(52.52, 13.405, 2026, 6, 21, rise, set));
    CHECK(std::abs(local_of(rise, 120) - (4 * 60 + 43)) <= 3);
    CHECK(std::abs(local_of(set, 120) - (21 * 60 + 33)) <= 3);
    REQUIRE(sun_utc_minutes(52.52, 13.405, 2026, 12, 21, rise, set));
    CHECK(std::abs(local_of(rise, 60) - (8 * 60 + 16)) <= 3);
    CHECK(std::abs(local_of(set, 60) - (15 * 60 + 54)) <= 3);
}

TEST_CASE("day length follows the seasons")
{
    double rise = 0;
    double set = 0;
    // At the equator the day is about 12 hours all year.
    REQUIRE(sun_utc_minutes(0.0, 0.0, 2026, 3, 20, rise, set));
    CHECK(set - rise > 12 * 60);
    CHECK(set - rise < 12 * 60 + 15);
    // Berlin: long in June, short in December.
    double rise_june = 0;
    double set_june = 0;
    double rise_dec = 0;
    double set_dec = 0;
    REQUIRE(sun_utc_minutes(52.5, 13.4, 2026, 6, 21, rise_june, set_june));
    REQUIRE(sun_utc_minutes(52.5, 13.4, 2026, 12, 21, rise_dec, set_dec));
    CHECK(set_june - rise_june > 16 * 60);
    CHECK(set_dec - rise_dec < 8 * 60 + 15);
    // Leap year and the end of the year do not break the day number.
    CHECK(sun_utc_minutes(52.5, 13.4, 2028, 2, 29, rise, set));
    CHECK(sun_utc_minutes(52.5, 13.4, 2026, 12, 31, rise, set));
}

TEST_CASE("polar day and night have no sunrise")
{
    double rise = 0;
    double set = 0;
    CHECK_FALSE(sun_utc_minutes(80.0, 15.0, 2026, 6, 21, rise, set));
    CHECK_FALSE(sun_utc_minutes(80.0, 15.0, 2026, 12, 21, rise, set));
    CHECK_FALSE(sun_utc_minutes(52.5, 13.4, 2026, 13, 1, rise, set));
}

TEST_CASE("local minutes wrap around midnight")
{
    CHECK(local_minutes(0, 0) == 0);
    CHECK(local_minutes(1430, 60) == 50);
    CHECK(local_minutes(-30, 0) == 1410);
    CHECK(local_minutes(10, -60) == 1390);
}
