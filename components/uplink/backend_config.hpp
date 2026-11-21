#pragma once

// Limits of the sync with the own backend: how much one check-in carries,
// how many thumbnails to ask for, the video size limits (backend*.cpp).

#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

namespace uplink::backend
{

constexpr size_t kUploadChunk = 4096;
// Keep a check-in well below the backend's limits (16 cameras, 2000 media);
// media not sent now go with the next sync.
constexpr std::size_t kMaxCamerasPerCheckin = 16;
#if CONFIG_SPIRAM
constexpr std::size_t kMaxMediaPerCamera = 300;
constexpr std::size_t kMaxMediaPerCheckin = 1000;
#else
// Without PSRAM the check-in has to fit next to the TLS session in about
// 80 KB of heap. A recording costs about 140 B there (its entry in the
// request and in the JSON); 300 of them left 7 KB free on the WROOM-32D,
// not enough for the response (#244). The backend asks for fewer thumbnails
// per check-in than that anyway, so a backlog does not reach the phone later
// because of this limit.
constexpr std::size_t kMaxMediaPerCamera = 40;
constexpr std::size_t kMaxMediaPerCheckin = 40;
#endif
#if CONFIG_SPIRAM
// Thumbnail uploads per check-in: left to the backend (30).
constexpr std::int64_t kMaxThumbUploads = -1;
#else
// The backend answers with a presigned URL per thumbnail, and the response
// arrives as one TLS record that mbedTLS needs in one block. On the
// WROOM-32D the record for 4 uploads was 7.6 KB (about 1.8 KB per upload)
// and did not always fit: "alloc(7646 bytes) failed" in one cycle, fine in
// the next (#244). Two should give about 4 KB; the rest of a backlog follows
// with the next check-ins, newest first.
constexpr std::int64_t kMaxThumbUploads = 2;
#endif
constexpr std::size_t kVideoChunk = 16 * 1024;

#if CONFIG_CAMERA_DOWNLOAD_VIDEOS
constexpr bool kDownloadsVideos = true;
constexpr std::uint64_t kMaxVideoBytes =
    (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB * 1024 * 1024;
#else
constexpr bool kDownloadsVideos = false;
constexpr std::uint64_t kMaxVideoBytes = 50ull * 1024 * 1024;
#endif
#if CONFIG_CAMERA_DOWNLOAD_VIDEOS && CONFIG_MODEM_UPLINK
constexpr std::uint64_t kMaxVideoBytesModem =
    (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB_MODEM * 1024 * 1024;
#else
constexpr std::uint64_t kMaxVideoBytesModem = kMaxVideoBytes;
#endif

}  // namespace uplink::backend
