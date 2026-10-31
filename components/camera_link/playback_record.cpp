#include "playback_record.hpp"

#include <cstring>

namespace camera::artemis
{

namespace
{

constexpr std::size_t kPageSize = 0x1000;
constexpr std::size_t kEncryptedPrefix = 0x60;

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Position of the first 00 00 00 01, else of the first 00 00 01, else size.
std::size_t find_start_code(const std::uint8_t *data, std::size_t size)
{
    static const std::uint8_t kLong[] = {0, 0, 0, 1};
    static const std::uint8_t kShort[] = {0, 0, 1};
    for (std::size_t i = 0; i + sizeof(kLong) <= size; i++) {
        if (std::memcmp(data + i, kLong, sizeof(kLong)) == 0)
            return i;
    }
    for (std::size_t i = 0; i + sizeof(kShort) <= size; i++) {
        if (std::memcmp(data + i, kShort, sizeof(kShort)) == 0)
            return i;
    }
    return size;
}

}  // namespace

bool parse_playback_record(std::uint8_t *payload, std::size_t size,
                           PlaybackFrame &out)
{
    out = PlaybackFrame();
    if (payload == nullptr || size < kPlaybackHeaderSize) {
        return false;
    }
    const std::size_t data_size = size - kPlaybackHeaderSize;
    const std::uint32_t width = read_u32_le(payload + 28);
    const std::uint32_t height = read_u32_le(payload + 32);
    if (data_size > 0 && read_u32_le(payload + 16) == data_size) {
        out.kind = width != 0 && height != 0 ? PlaybackKind::video
                                             : PlaybackKind::other;
    } else if (data_size > 0 && read_u32_le(payload + 20) == data_size) {
        out.kind = width == 0 && height == 0 ? PlaybackKind::audio
                                             : PlaybackKind::other;
    } else {
        return false;
    }
    out.pts_ms = read_u32_le(payload + 8);
    out.width = width;
    out.height = height;
    out.session_no = read_u32_le(payload + 48);
    out.data = payload + kPlaybackHeaderSize;
    out.size = data_size;
    return true;
}

void decrypt_playback_data(const Aes128 &cipher, std::uint8_t *data,
                           std::size_t size)
{
    for (std::size_t page = 0; page < size; page += kPageSize) {
        // As the app's native code: only when more than 0x5f bytes remain.
        if (size - page < kEncryptedPrefix) {
            continue;
        }
        std::uint8_t previous[Aes128::kBlockSize] = {};
        for (std::size_t block = 0; block < kEncryptedPrefix;
             block += Aes128::kBlockSize) {
            std::uint8_t *at = data + page + block;
            std::uint8_t cipher_block[Aes128::kBlockSize];
            std::memcpy(cipher_block, at, sizeof(cipher_block));
            cipher.decrypt_block(cipher_block, at);
            for (std::size_t i = 0; i < Aes128::kBlockSize; i++) {
                at[i] ^= previous[i];
            }
            std::memcpy(previous, cipher_block, sizeof(previous));
        }
    }
}

bool is_marker_frame(const std::uint8_t *data, std::size_t size)
{
    static const std::uint8_t kMarker[] = {0, 0, 0, 1, 1};
    return size >= sizeof(kMarker) &&
           std::memcmp(data, kMarker, sizeof(kMarker)) == 0;
}

std::size_t annexb_start_offset(const std::uint8_t *data, std::size_t size)
{
    const std::size_t start = find_start_code(data, size);
    return start <= 4 && start < size ? start : 0;
}

}  // namespace camera::artemis
