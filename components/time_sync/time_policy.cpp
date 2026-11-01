#include "time_policy.hpp"

#include <cstring>

namespace timepolicy
{

namespace
{

constexpr std::int64_t kClockValidFrom = 1704067200;  // 2024-01-01
constexpr std::int64_t kYear2100 = 4102444800;
constexpr std::int64_t kTenYears = 10LL * 365 * 86400;

// Days since 1970-01-01 of a civil date (proleptic Gregorian calendar).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool read_number(const char *&p, int digits, int &value)
{
    value = 0;
    for (int i = 0; i < digits; i++, p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (*p - '0');
    }
    return true;
}

}  // namespace

const char *source_name(Source source)
{
    switch (source) {
    case Source::camera:
        return "camera";
    case Source::browser:
        return "browser";
    case Source::backend:
        return "backend";
    case Source::sntp:
        return "sntp";
    case Source::none:
        break;
    }
    return "none";
}

bool trusted(Source source)
{
    return static_cast<int>(source) >= static_cast<int>(Source::browser);
}

bool may_set(Source current, Source incoming, bool clock_valid)
{
    return !clock_valid ||
           static_cast<int>(incoming) >= static_cast<int>(current);
}

bool plausible(std::int64_t unix_seconds, std::int64_t build_time)
{
    std::int64_t earliest = kClockValidFrom;
    std::int64_t latest = kYear2100;
    if (build_time - 86400 > earliest) {
        earliest = build_time - 86400;
    }
    if (build_time > 0 && build_time + kTenYears < latest) {
        latest = build_time + kTenYears;
    }
    return unix_seconds >= earliest && unix_seconds < latest;
}

std::int64_t parse_build_time(const char *date, const char *time)
{
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (date == nullptr || time == nullptr || std::strlen(date) < 11 ||
        std::strlen(time) < 8) {
        return 0;
    }
    unsigned month = 0;
    for (unsigned i = 0; i < 12; i++) {
        if (std::strncmp(date, kMonths + 3 * i, 3) == 0) {
            month = i + 1;
        }
    }
    const char *p = date + 4;
    int day = 0;
    if (*p == ' ') {
        p++;
        if (!read_number(p, 1, day)) {
            return 0;
        }
    } else if (!read_number(p, 2, day)) {
        return 0;
    }
    int year = 0;
    p++;  // the space
    if (!read_number(p, 4, year)) {
        return 0;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    const char *t = time;
    if (month == 0 || !read_number(t, 2, hour) || *t++ != ':' ||
        !read_number(t, 2, minute) || *t++ != ':' ||
        !read_number(t, 2, second)) {
        return 0;
    }
    return days_from_civil(year, month, static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

}  // namespace timepolicy
