#pragma once

// Build options and constants of the camera session, shared by the files that
// make up camera_session.hpp's class. Private to this component.

#include <cstddef>
#include <cstdint>

#include "media_catalog.hpp"
#include "sdkconfig.h"

#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500
// Frames buffered ahead of a missing one while streaming a download (1 KB
// each); later frames are not acknowledged, so the camera resends them.
// The window is one block of up to 1472 bytes per frame, in PSRAM if there is
// one; without, internal RAM is too short for more. During a video playback
// the camera sends about 120 frames ahead of the first unacknowledged one, so
// the window must be larger than that: with 32 frames, most of them were
// dropped and sent again, which made the download about 20 times slower than
// playback (256 frames take about 377 KB of PSRAM per stream).
#if CONFIG_SPIRAM
#define CAMERA_DOWNLOAD_REORDER_FRAMES 256
#else
#define CAMERA_DOWNLOAD_REORDER_FRAMES 8
#endif
#define CAMERA_BULK_ACK_DELAY_US (1 * 1000LL)
// A thumbnail with no new frame for this long, or whose request brought no
// frame at all for this long, is requested again.
#define CAMERA_THUMBNAIL_STALL_US (3 * 1000 * 1000LL)
// Log a download's progress every this many bytes.
#define CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES (256u * 1024u)

#ifdef CONFIG_CAMERA_DOWNLOAD_PHOTOS
static const bool kDownloadPhotos = true;
#else
static const bool kDownloadPhotos = false;
#endif
// Videos are not offered by the file download (cmdId 1285): the camera
// acknowledges the request but sends nothing. They are fetched by playing
// them back (cmdId 769), which streams their H.264 and AAC frames.
#if defined(CONFIG_CAMERA_DOWNLOAD_VIDEOS) &&                                  \
    defined(CONFIG_CAMERA_VIDEO_ON_REQUEST)
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::requested;
#elif defined(CONFIG_CAMERA_DOWNLOAD_VIDEOS)
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::missing;
#else
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::none;
#endif
static const bool kDownloadVideos =
    kVideoDownload != camera::artemis::MediaCatalog::VideoDownload::none;
// Camera answers up to this length are logged in full at INFO (after
// json_redact); longer ones only by size, and in full at DEBUG. With the log
// prefix this still fits a line of the SD log (SD_LOG_LINE_MAX, 256 bytes).
static constexpr size_t kResponseLogInfoMax = 200;
// Videos fetched in one session (0: no limit); each takes about twice its
// length.
#ifdef CONFIG_CAMERA_VIDEO_MAX_PER_SESSION
#define CAMERA_VIDEO_MAX_PER_SESSION CONFIG_CAMERA_VIDEO_MAX_PER_SESSION
#else
#define CAMERA_VIDEO_MAX_PER_SESSION 0
#endif
// Throughput test: receive and parse videos without writing them to the SD
// card (nothing is stored or marked as downloaded).
#ifdef CONFIG_CAMERA_VIDEO_DISCARD_DATA
static const bool kDiscardVideoData = true;
#else
static const bool kDiscardVideoData = false;
#endif
#ifdef CONFIG_CAMERA_THUMBNAIL_SERIAL_PREVIEW
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 1
#else
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 0
#endif
#ifndef CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
#define CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS                              \
    CAMERA_RUDP_HEARTBEAT_INTERVAL_MS
#endif
#ifndef CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S
#define CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S 20
#endif
#ifndef CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S
#define CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S 60
#endif
#if CONFIG_CAMERA_DOWNLOAD_PHOTOS || CONFIG_CAMERA_DOWNLOAD_VIDEOS
// Otherwise the session closes before a stalled download is given up and the
// next item requested (#106).
static_assert(CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S >
                  CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S,
              "CAMERA_SESSION_IDLE_TIMEOUT_S must be above "
              "CAMERA_DOWNLOAD_STALL_TIMEOUT_S");
#endif

[[maybe_unused]] static const char *TAG = "trailcamhub_protocol";
// The camera answers an action within a second or two; 518 took 0.9 s.
static constexpr int64_t kActionAnswerTimeoutUs = 15 * 1000 * 1000;
