#include "camera_prelogin.hpp"

#include <cstdio>
#include <cstring>

#include "artemis_aes128.hpp"

namespace camera
{

namespace
{

// The first 28 bytes of the TrailCam Go app's 0xF9 packet to the CS2 P2P
// servers (119.23.219.190, 47.52.46.203, 121.43.173.0, UDP 32100), copied by
// pi_trailcam. In the 27 such packets of pwnb0t/trailcam-automation the first
// 23 bytes are always these; the last five differ per app session (four
// variants; this one in 15 packets). The app never sends 0xF9 to the camera,
// and the bytes do not decrypt with the PRE_LOGIN or the command key: they
// are the app's server login, not something the camera checks (#51).
// clang-format off
const std::uint8_t kPreLoginHeader[kPreLoginHeaderSize] = {
    0x0c, 0xcb, 0x9a, 0x2b, 0x5f, 0x95, 0x1e, 0xb6,
    0x69, 0xdf, 0xaa, 0x37, 0x5a, 0x6b, 0xbe, 0x3e,
    0x76, 0x20, 0x2e, 0x13, 0xc9, 0xd1, 0xaa, 0x36,
    0x31, 0xbe, 0x74, 0xe5,
};
// clang-format on
const std::uint8_t kPreLoginKey[artemis::Aes128::kKeySize] = {
    'a', '0', '1', 'b', 'c', '2', '3', 'e',
    'd', '4', '5', 'f', 'F', '5', '6', 'A',
};

}  // namespace

std::size_t pkcs7_pad(std::uint8_t *data, std::size_t length,
                      std::size_t capacity)
{
    const std::size_t padding = kAesBlockSize - (length % kAesBlockSize);
    if (data == nullptr || length + padding > capacity) {
        return 0;
    }
    std::memset(data + length, static_cast<int>(padding), padding);
    return length + padding;
}

std::size_t build_prelogin_plaintext(std::int64_t utc_time,
                                     const std::uint8_t *nonce,
                                     std::uint8_t *out, std::size_t capacity,
                                     std::size_t *json_length)
{
    if (nonce == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    const int written = std::snprintf(
        reinterpret_cast<char *>(out), capacity,
        "{\"utcTime\":%lld,\"nonce\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}",
        static_cast<long long>(utc_time), nonce[0], nonce[1], nonce[2],
        nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity) {
        return 0;
    }
    const std::size_t length = static_cast<std::size_t>(written);
    if (json_length != nullptr) {
        *json_length = length;
    }
    return pkcs7_pad(out, length, capacity);
}

std::size_t build_prelogin_body(std::int64_t utc_time,
                                const std::uint8_t *nonce, std::uint8_t *out,
                                std::size_t capacity, std::size_t *json_length)
{
    if (out == nullptr || capacity < kPreLoginHeaderSize) {
        return 0;
    }
    std::uint8_t plaintext[kPreLoginMaxBodySize - kPreLoginHeaderSize];
    const std::size_t plaintext_length = build_prelogin_plaintext(
        utc_time, nonce, plaintext, sizeof(plaintext), json_length);
    if (plaintext_length == 0 ||
        kPreLoginHeaderSize + plaintext_length > capacity) {
        std::memset(plaintext, 0, sizeof(plaintext));
        return 0;
    }
    std::memcpy(out, kPreLoginHeader, kPreLoginHeaderSize);
    const artemis::Aes128 cipher(kPreLoginKey);
    for (std::size_t offset = 0; offset < plaintext_length;
         offset += kAesBlockSize) {
        cipher.encrypt_block(plaintext + offset,
                             out + kPreLoginHeaderSize + offset);
    }
    std::memset(plaintext, 0, sizeof(plaintext));
    return kPreLoginHeaderSize + plaintext_length;
}

}  // namespace camera
