#include "sun.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace schedule
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kZenith = 90.833;

double radians(double degrees)
{
    return degrees * kPi / 180.0;
}

double degrees(double radians_value)
{
    return radians_value * 180.0 / kPi;
}

double wrap(double value, double range)
{
    value = std::fmod(value, range);
    return value < 0 ? value + range : value;
}

int day_of_year(int year, int month, int day)
{
    static const int kCumulative[] = {0,   31,  59,  90,  120, 151,
                                      181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int n = kCumulative[month - 1] + day;
    if (leap && month > 2) {
        n++;
    }
    return n;
}

// UT in hours of the sun crossing the horizon; false if it does not.
bool crossing(double latitude, double longitude, int day_number, bool rising,
              double &hours)
{
    const double lng_hour = longitude / 15.0;
    const double t = day_number + ((rising ? 6.0 : 18.0) - lng_hour) / 24.0;
    const double mean = 0.9856 * t - 3.289;
    const double lambda =
        wrap(mean + 1.916 * std::sin(radians(mean)) +
                 0.020 * std::sin(radians(2 * mean)) + 282.634,
             360.0);
    double ra =
        wrap(degrees(std::atan(0.91764 * std::tan(radians(lambda)))), 360.0);
    ra += std::floor(lambda / 90.0) * 90.0 - std::floor(ra / 90.0) * 90.0;
    ra /= 15.0;
    const double sin_dec = 0.39782 * std::sin(radians(lambda));
    const double cos_dec = std::cos(std::asin(sin_dec));
    const double cos_h =
        (std::cos(radians(kZenith)) - sin_dec * std::sin(radians(latitude))) /
        (cos_dec * std::cos(radians(latitude)));
    if (cos_h > 1.0 || cos_h < -1.0) {
        return false;
    }
    double h = degrees(std::acos(cos_h));
    if (rising) {
        h = 360.0 - h;
    }
    h /= 15.0;
    const double local_mean = h + ra - 0.06571 * t - 6.622;
    hours = wrap(local_mean - lng_hour, 24.0);
    return true;
}

}  // namespace

bool parse_location(const std::string &text, Location &out, std::string *error)
{
    auto fail = [error](const char *reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };
    std::string first;
    std::string second;
    std::string *current = &first;
    for (char c : text) {
        if ((c == ',' || c == ';' || c == ' ') && current == &first &&
            !first.empty()) {
            current = &second;
            continue;
        }
        if (c == ' ' && current == &second && second.empty()) {
            continue;
        }
        if (c == ',' || c == ';' || c == ' ') {
            return fail("location must look like 52.5,13.4");
        }
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
            return fail("location must look like 52.5,13.4");
        }
        *current += c;
    }
    if (first.empty() || second.empty()) {
        return fail("location must look like 52.5,13.4");
    }
    char *end = nullptr;
    const double latitude = std::strtod(first.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fail("location must look like 52.5,13.4");
    }
    const double longitude = std::strtod(second.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fail("location must look like 52.5,13.4");
    }
    if (latitude < -90.0 || latitude > 90.0) {
        return fail("latitude must be between -90 and 90");
    }
    if (longitude < -180.0 || longitude > 180.0) {
        return fail("longitude must be between -180 and 180");
    }
    Location location;
    location.latitude = std::round(latitude * 10.0) / 10.0;
    location.longitude = std::round(longitude * 10.0) / 10.0;
    location.valid = true;
    out = location;
    return true;
}

std::string format_location(const Location &location)
{
    if (!location.valid) {
        return std::string();
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f,%.1f", location.latitude + 0.0,
                  location.longitude + 0.0);
    return text;
}

bool sun_utc_minutes(double latitude, double longitude, int year, int month,
                     int day, double &sunrise, double &sunset)
{
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    const int n = day_of_year(year, month, day);
    double rise = 0;
    double set = 0;
    if (!crossing(latitude, longitude, n, true, rise) ||
        !crossing(latitude, longitude, n, false, set)) {
        return false;
    }
    sunrise = rise * 60.0;
    sunset = set * 60.0;
    // Keep both in the order of the local day: the sun sets after it rose.
    if (sunset < sunrise) {
        sunset += 1440.0;
    }
    return true;
}

int local_minutes(double utc_minutes, int offset_min)
{
    const double value = wrap(utc_minutes + offset_min, 1440.0);
    return static_cast<int>(std::floor(value + 0.5)) % 1440;
}

}  // namespace schedule
