#pragma once

// Which source may set the clock of the device. Pure logic, host-testable.
//
// The clock can come from several places that are not equally good: SNTP and
// the backend know the time, the browser of someone who opens the web UI
// shows what their computer thinks, and the camera has the clock that it was
// last given (it is wrong if a firmware or app has set it wrong). A better
// source replaces a worse one; a worse one sets the clock only while it has
// not been set at all.

#include <cstdint>

namespace timepolicy
{

enum class Source : int {
    none = 0,
    camera = 1,
    browser = 2,
    backend = 3,
    sntp = 4,
};

const char *source_name(Source source);

// True for a source that is good enough to give its time to the camera: the
// camera's own time must not come back to it, as that would only confirm an
// error.
bool trusted(Source source);

// May `incoming` set the clock? `clock_valid` is whether it holds a time.
bool may_set(Source current, Source incoming, bool clock_valid);

// A time that can be right: not before 2024, not before the build of this
// firmware (less a day for the time zone) and not more than ten years after
// it; without a build time, before 2100. `build_time` is 0 if unknown.
bool plausible(std::int64_t unix_seconds, std::int64_t build_time);

// Seconds since the epoch of a compiler's __DATE__ ("Oct 31 2026", the day
// padded with a space) and __TIME__ ("21:21:50"), read as UTC; 0 if they
// cannot be read.
std::int64_t parse_build_time(const char *date, const char *time);

}  // namespace timepolicy
