#pragma once

// Parsing of hexadecimal byte strings such as "42 54:5F-4B". This module has
// no ESP-IDF dependencies so it can be unit-tested on the host.

#include <cstddef>
#include <cstdint>

namespace camera
{

// Parses pairs of hex digits, optionally separated by whitespace, ':' or '-'.
// Returns false for odd digit counts, invalid characters or if the result
// does not fit into `capacity` bytes. An empty string yields zero bytes.
bool parse_hex_payload(const char *text, std::uint8_t *out,
                       std::size_t capacity, std::size_t *out_len);

}  // namespace camera
