#pragma once

// Sunrise and sunset for the camera read windows (#436). Pure logic,
// host-testable.
//
// The algorithm is the almanac one (zenith 90.833 degrees, which includes
// refraction and the sun's radius): about +-2 minutes in Central Europe, far
// enough for read windows that are an hour wide.

#include <cstdint>
#include <functional>
#include <string>

namespace schedule
{

// The hub's position, rounded to 0.1 degrees (about 10 km: the sun times
// change by less than a minute over that).
struct Location {
    double latitude = 0;
    double longitude = 0;
    bool valid = false;
};

// Used where no location is set: the middle of Germany.
constexpr double kFallbackLatitude = 51.0;
constexpr double kFallbackLongitude = 10.0;

// Parses "LAT,LON" in decimal degrees (comma, space or semicolon between;
// latitude -90..90, longitude -180..180), rounded to 0.1. An empty text is not
// valid (no location). Returns false with a short English reason in `error`.
bool parse_location(const std::string &text, Location &out, std::string *error);

// "52.5,13.4"; empty for an invalid location.
std::string format_location(const Location &location);

// Sunrise and sunset of a date at a position as minutes of the day in UTC,
// possibly below 0 or above 1439 (the sun can rise before midnight UTC
// far in the east or west). False if there is none that day (polar day or
// night).
bool sun_utc_minutes(double latitude, double longitude, int year, int month,
                     int day, double &sunrise, double &sunset);

// Sunrise and sunset of the local day as local minutes of the day (0-1439);
// -1 when unknown (no clock) or there is none.
struct SunDay {
    int sunrise = -1;
    int sunset = -1;
};

// The sun times of the local day that contains a schedule time. An empty
// function: no sun times (windows that need them are skipped).
using SunMinutes = std::function<SunDay(std::int64_t)>;

// `utc_minutes` shifted by `offset_min` (local time) into 0-1439.
int local_minutes(double utc_minutes, int offset_min);

}  // namespace schedule
