#pragma once

// Pure parts of the WebDAV destination (docs/modes.md): file names and URLs
// on the WebDAV share. Host-testable.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace uplink
{
namespace push
{

// "2026-11-08_15-04-05_1176197582.jpg" from the camera's wall clock (read
// as UTC, see docs/clock.md); without a time "1176197582.jpg".
std::string file_name(std::int64_t media_time, std::int64_t media_id,
                      const char *extension);

// "2026-11-08 15:04" from the camera's wall clock; empty without a time.
std::string wall_time(std::int64_t media_time);

// Percent-encodes one path segment (RFC 3986 unreserved characters stay).
std::string encode_segment(const std::string &segment);

// `base` + "/" + the encoded segments, with exactly one slash between them.
std::string join_url(const std::string &base,
                     const std::vector<std::string> &segments);

}  // namespace push
}  // namespace uplink
