#pragma once

// Pure, host-testable building blocks for transferring media off the camera:
// the control-plane request bodies that ask for a thumbnail (cmdId 772) or a
// full file (cmdId 1285), and the parser for the binary media record the
// camera streams back on the bulk data channel. The bulk transport itself
// (D0/D1 subtype routing, 16-bit sequence reassembly, windowed ACKs) lives in
// the runtime; this module is just the codec.

#include <cstddef>
#include <cstdint>
#include <string>

#include "media_list.hpp"

namespace camera::artemis
{

// --- Control-plane request bodies ------------------------------------------
//
// Each returns the plain JSON object that the ARTEMIS layer then encrypts and
// frames (same path as the login/list commands). fileType is 0 for photos and
// 1 for videos; dir_num is the DCIM directory (the list's mediaDirNum).

// Thumbnail request (cmdId 772). The thumbnail JPEG streams back on the bulk
// channel (commonly D0 subtype 0x04).
std::string build_thumbnail_request(std::int64_t file_type,
                                    std::int64_t dir_num,
                                    std::int64_t media_num, std::int64_t token);
std::string build_thumbnail_request(const MediaItem &item, std::int64_t token);

// Full-file download request (cmdId 1285). The camera first answers with a
// small JSON ACK ({"cmdRet":0,"result":0,"cmdId":1285}); the bytes then stream
// back on the bulk channel (D0 subtype 0x03 for photos).
std::string build_download_request(std::int64_t file_type, std::int64_t dir_num,
                                   std::int64_t media_num, std::int64_t token);
std::string build_download_request(const MediaItem &item, std::int64_t token);

// --- Bulk media record ------------------------------------------------------
//
// A reassembled bulk record carries the same 20-byte ARTEMIS header as the
// control plane, but its payload is binary: a fixed 72-byte media header
// followed by the raw JPEG bytes.

constexpr std::size_t kMediaRecordHeaderSize = 72;

struct MediaRecordHeader {
    std::int64_t dir_num = 0;    // payload[0x20:0x22], little-endian
    std::int64_t media_num = 0;  // payload[0x22:0x24], little-endian
    std::int64_t media_id = 0;   // payload[0x30:0x34], little-endian
};

// Parses the media header at the front of a bulk record payload and locates
// the embedded JPEG. Returns false if the payload is shorter than the header.
// On success jpeg_offset == kMediaRecordHeaderSize and jpeg_size covers the
// remaining bytes (which may be 0 for a header-only record).
bool parse_media_record(const std::uint8_t *payload, std::size_t payload_size,
                        MediaRecordHeader &out, std::size_t &jpeg_offset,
                        std::size_t &jpeg_size);

}  // namespace camera::artemis
