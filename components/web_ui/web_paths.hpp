#pragma once

// Request parsing helpers of the web UI. No ESP-IDF dependencies, so they can
// be unit-tested on the host.

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

// Appends `text` as a JSON string literal (with quotes).
void append_json_string(std::string &out, const std::string &text);

}  // namespace web
