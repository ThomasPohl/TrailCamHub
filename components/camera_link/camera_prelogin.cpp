#include "camera_prelogin.hpp"

#include <cstdio>
#include <cstring>

namespace camera
{

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

}  // namespace camera
