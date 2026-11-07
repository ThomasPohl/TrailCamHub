#pragma once

// What the hub does with a "requestVideo" command from the backend, from the
// state of the recording in the camera's catalog on the SD card. Pure logic,
// host-testable; the upload itself lives in uplink.cpp.
//
// A request is answered (acknowledged) only when it is settled: the video
// was uploaded, or it cannot be fetched. Until then the backend sends the
// command again with every check-in, each time with a fresh upload URL, and
// the hub answers nothing.

#include <cstdint>
#include <string>

#include "media_catalog.hpp"

namespace uplink
{

enum class VideoStep {
    // Not answered yet; the catalog was flagged now: a camera read is due.
    request,
    // Not answered yet; already flagged, waiting for the next camera read.
    wait,
    // The streams are on the SD card: upload the MP4, then answer "done".
    upload,
    // Uploaded before: answer "done".
    done,
    // Answer "failed" (see VideoFailure).
    fail,
};

enum class VideoFailure {
    none,
    unsupported,    // this firmware build does not download videos
    not_on_hub,     // the hub does not know the recording
    not_a_video,    // the recording is a photo
    not_on_camera,  // no longer on the camera's card
    gave_up,        // its download failed kMaxVideoAttempts times
    no_url,         // nothing to upload to (backend too old)
};

struct VideoDecision {
    VideoStep step = VideoStep::wait;
    VideoFailure failure = VideoFailure::none;
};

// `catalog`: the camera's catalog on the SD card, or null if the hub has no
// data for the camera. `downloads`: this build downloads videos.
// `has_upload_url`: the command carries an upload URL.
VideoDecision decide_video(const camera::artemis::MediaCatalog *catalog,
                           std::int64_t media_id, bool downloads,
                           bool has_upload_url);

const char *video_failure_text(VideoFailure failure);

// Largest MP4 the hub uploads, per transport (bytes).
struct VideoLimits {
    std::uint64_t wifi_bytes = 0;
    // Over the cellular modem (data volume, radio time).
    std::uint64_t modem_bytes = 0;
};

// Checked once the MP4's size is known, before anything is sent. Empty if
// the video may be uploaded over this transport, else the reason to answer
// "failed" with. A hub syncs over one transport only (setting "transport"),
// so a video too large for the modem is not kept for a later Wi-Fi sync.
std::string video_size_failure(std::uint64_t size, const VideoLimits &limits,
                               bool over_modem);

}  // namespace uplink
