#pragma once

// Request parsing helpers of the web UI. No ESP-IDF dependencies, so they can
// be unit-tested on the host.

#include <cstdint>
#include <string>

namespace web
{

// Decodes %XX escapes and '+' (as space). Returns false on a malformed escape
// or a decoded NUL byte.
bool url_decode(const std::string &in, std::string &out);

// Turns a client-supplied path below the SD card root into a normalized
// relative path ("" for the root, "a/b" otherwise). Rejects ".." segments,
// backslashes and control characters; empty segments are dropped.
bool sanitize_path(const std::string &in, std::string &out);

// True for files that the firmware is still writing and renames when done:
// downloads (".part") and catalog updates (".tmp").
bool is_temporary_file(const std::string &name);

// MIME type for a file name, by extension.
const char *content_type(const std::string &name);

// For the path of an MP4 file ("cam/media/100_10.mp4"), the paths of the raw
// streams it is built from ("…/100_10.h264" and "…/100_10.aac"). False if the
// path is not that of an MP4 file.
bool mp4_sources(const std::string &mp4_path, std::string &video,
                 std::string &audio);

enum class RangeResult {
    kNone,           // no usable Range header: send the whole file
    kPartial,        // send bytes first..last
    kUnsatisfiable,  // the range lies beyond the end of the file
};

// Parses the value of a "Range: bytes=..." header for a file of `size` bytes
// (one range only; "a-b", "a-" and "-n"). `last` is inclusive.
RangeResult parse_range(const std::string &header, std::uint64_t size,
                        std::uint64_t &first, std::uint64_t &last);

// Reads a field of a flat JSON object such as a camera's camera.json: an
// integer, or a string with the backslash escapes \" and \\. False if the
// key is absent or has another type.
bool json_int(const std::string &json, const std::string &key,
              long long &value);
bool json_string(const std::string &json, const std::string &key,
                 std::string &value);

// Appends `text` as a JSON string literal (with quotes).
void append_json_string(std::string &out, const std::string &text);

}  // namespace web
