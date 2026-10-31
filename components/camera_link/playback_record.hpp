#pragma once

// Decodes the records of a video playback stream (ARTEMIS version 4, see
// docs/video-download.md): a 108-byte header followed by the frame data. In
// every 0x1000-byte page of the data the first 0x60 bytes are AES-128-CBC
// encrypted with the command key and a zero IV; a shorter tail is plain.
// Video data is H.264 in Annex-B form, audio data AAC with ADTS headers.
// Pure logic, host-testable.

#include <cstddef>
#include <cstdint>

#include "artemis_aes128.hpp"

namespace camera::artemis
{

constexpr std::uint32_t kPlaybackRecordVersion = 4;
constexpr std::size_t kPlaybackHeaderSize = 108;

enum class PlaybackKind : std::uint8_t {
    video,  // data length at offset 16, width and height set
    audio,  // data length at offset 20, no width and height
    other,
};

struct PlaybackFrame {
    PlaybackKind kind = PlaybackKind::other;
    std::uint32_t pts_ms = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t session_no = 0;
    std::uint8_t *data = nullptr;  // points into the record payload
    std::size_t size = 0;
};

// Parses the header of a version-4 record payload. Returns false if the
// payload is too short or neither length field matches the data size.
bool parse_playback_record(std::uint8_t *payload, std::size_t size,
                           PlaybackFrame &out);

// Decrypts the encrypted prefix of every page of `data` in place.
void decrypt_playback_data(const Aes128 &cipher, std::uint8_t *data,
                           std::size_t size);

// The app drops a marker frame that starts with 00 00 00 01 01.
bool is_marker_frame(const std::uint8_t *data, std::size_t size);

// Offset of the first H.264 start code if it is within the first four bytes
// (a short alignment prefix to drop), otherwise 0. A start code further in is
// left alone: the bytes before it may belong to the previous access unit.
std::size_t annexb_start_offset(const std::uint8_t *data, std::size_t size);

}  // namespace camera::artemis
