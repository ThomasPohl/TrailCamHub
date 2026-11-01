#include "camera_clock.hpp"

#include <time.h>

namespace camera
{

namespace
{

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

}  // namespace

std::int64_t camera_wall_clock(std::int64_t utc)
{
    const time_t instant = static_cast<time_t>(utc);
    struct tm local = {};
    if (localtime_r(&instant, &local) == nullptr) {
        return utc;
    }
    // The local calendar time, read as if it were UTC.
    return days_from_civil(local.tm_year + 1900,
                           static_cast<unsigned>(local.tm_mon + 1),
                           static_cast<unsigned>(local.tm_mday)) *
               86400 +
           local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
}

std::int64_t camera_wall_clock_to_utc(std::int64_t wall)
{
    // The offset in effect if `wall` were UTC, then the one at the result.
    const std::int64_t first = camera_wall_clock(wall) - wall;
    std::int64_t utc = wall - first;
    const std::int64_t second = camera_wall_clock(utc) - utc;
    if (second != first) {
        utc = wall - second;
    }
    return utc;
}

std::int64_t media_file_time(std::int64_t media_time)
{
    if (media_time <= 0) {
        return 0;
    }
    return camera_wall_clock_to_utc(media_time);
}

}  // namespace camera
