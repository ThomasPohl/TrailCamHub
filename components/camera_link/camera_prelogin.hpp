#pragma once

// PRE_LOGIN payload: a static header followed by JSON with the UTC time and a
// random nonce, padded with PKCS#7 and encrypted with AES-128-ECB. This module
// has no ESP-IDF dependencies so it can be unit-tested on the host; the packet
// framing is PreLoginRequest (camera_udp_packets.hpp).

#include <cstddef>
#include <cstdint>

namespace camera
{

constexpr std::size_t kPreLoginNonceSize = 8;
constexpr std::size_t kAesBlockSize = 16;
// Bytes in front of the ciphertext: the start of the app's 0xF9 packet to
// the CS2 P2P servers (see camera_prelogin.cpp and #51).
constexpr std::size_t kPreLoginHeaderSize = 28;
// Largest payload build_prelogin_body() writes: the header and the encrypted
// plaintext of at most 96 bytes.
constexpr std::size_t kPreLoginMaxBodySize = kPreLoginHeaderSize + 96;

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

// Writes the PRE_LOGIN payload to `out`: the static header, then the
// plaintext of build_prelogin_plaintext() encrypted with AES-128-ECB under the
// PRE_LOGIN key. Returns the payload length, or 0 if `capacity` is too small.
// `json_length` (optional) receives the unpadded JSON length. The plaintext is
// wiped before returning.
std::size_t build_prelogin_body(std::int64_t utc_time,
                                const std::uint8_t *nonce, std::uint8_t *out,
                                std::size_t capacity, std::size_t *json_length);

}  // namespace camera
