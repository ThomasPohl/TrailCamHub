#pragma once

// The time that the camera is given at login. Pure logic, host-testable.

#include <cstdint>

namespace camera
{

// A clock before 2024 has not been set (the device has no battery-backed
// clock; it is set by SNTP or by the backend).
constexpr std::int64_t kClockValidFrom = 1704067200;

// The wall clock to set on the camera for the UTC time `utc`. The TrailCam Go
// app sends the local time of the phone as if it were UTC, and the camera
// shows it as it is (it has no time zone of its own). The local time comes
// from the time zone of the C library (the TZ variable, a POSIX rule such as
// "CET-1CEST,M3.5.0,M10.5.0/3"; the firmware sets it from CONFIG_TIME_ZONE at
// boot), so offset and summer time follow the rules of that zone.
std::int64_t camera_wall_clock(std::int64_t utc);

// The UTC time for a wall clock that the camera shows: the inverse of
// camera_wall_clock(), by the same time zone. During the hour that is shown
// twice when summer time ends, either of the two instants may come back.
std::int64_t camera_wall_clock_to_utc(std::int64_t wall);

}  // namespace camera
