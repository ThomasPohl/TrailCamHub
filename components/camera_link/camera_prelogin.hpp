#pragma once

// PRE_LOGIN plaintext: JSON with the UTC time and a random nonce, padded with
// PKCS#7 to the AES block size. This module has no ESP-IDF dependencies so it
// can be unit-tested on the host; encryption happens in camera_protocol.cpp.

#include <cstddef>
#include <cstdint>

namespace camera
{

constexpr std::size_t kPreLoginNonceSize = 8;
constexpr std::size_t kAesBlockSize = 16;

// Appends PKCS#7 padding to `data` (which holds `length` bytes) and returns
// the padded length, or 0 if `capacity` is too small.
std::size_t pkcs7_pad(std::uint8_t *data, std::size_t length,
                      std::size_t capacity);

// Writes {"utcTime":<utc_time>,"nonce":"<16 hex digits>"} followed by PKCS#7
// padding to `out`. Returns the padded length, or 0 if `capacity` is too
// small. `json_length` (optional) receives the unpadded JSON length.
std::size_t build_prelogin_plaintext(std::int64_t utc_time,
                                     const std::uint8_t *nonce,
                                     std::uint8_t *out, std::size_t capacity,
                                     std::size_t *json_length);

}  // namespace camera
