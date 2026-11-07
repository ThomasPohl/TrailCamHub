#include "video_request.hpp"

namespace uplink
{

namespace
{

VideoDecision fail(VideoFailure failure)
{
    return VideoDecision{VideoStep::fail, failure};
}

}  // namespace

VideoDecision decide_video(const camera::artemis::MediaCatalog *catalog,
                           std::int64_t media_id, bool downloads,
                           bool has_upload_url)
{
    using camera::artemis::MediaCatalog;
    const camera::artemis::CatalogEntry *entry =
        catalog != nullptr ? catalog->find(media_id) : nullptr;
    if (entry == nullptr) {
        return fail(VideoFailure::not_on_hub);
    }
    if (entry->file_type == 0) {
        return fail(VideoFailure::not_a_video);
    }
    const std::uint32_t flags = entry->flags;
    if ((flags & MediaCatalog::kVideoUploaded) != 0) {
        return VideoDecision{VideoStep::done, VideoFailure::none};
    }
    if ((flags & MediaCatalog::kFileDownloaded) != 0) {
        // Downloaded already (also without a request, e.g. "every video").
        return has_upload_url
                   ? VideoDecision{VideoStep::upload, VideoFailure::none}
                   : fail(VideoFailure::no_url);
    }
    if (!downloads) {
        return fail(VideoFailure::unsupported);
    }
    if ((flags & MediaCatalog::kVideoUnavailable) != 0) {
        return fail(catalog->video_attempts(media_id) >=
                            MediaCatalog::kMaxVideoAttempts
                        ? VideoFailure::gave_up
                        : VideoFailure::not_on_camera);
    }
    if ((flags & MediaCatalog::kVideoRequested) == 0) {
        return VideoDecision{VideoStep::request, VideoFailure::none};
    }
    return VideoDecision{VideoStep::wait, VideoFailure::none};
}

const char *video_failure_text(VideoFailure failure)
{
    switch (failure) {
    case VideoFailure::none:
        return "";
    case VideoFailure::unsupported:
        return "this firmware build does not download videos";
    case VideoFailure::not_on_hub:
        return "the hub does not know this recording";
    case VideoFailure::not_a_video:
        return "the recording is not a video";
    case VideoFailure::not_on_camera:
        return "the video is no longer on the camera's card";
    case VideoFailure::gave_up:
        return "the download from the camera failed three times";
    case VideoFailure::no_url:
        return "the backend gave no upload URL";
    }
    return "";
}

namespace
{

// Whole megabytes, rounded up, so that "16 MB, at most 15 MB" never reads
// "15 MB, at most 15 MB".
std::string megabytes(std::uint64_t bytes)
{
    return std::to_string((unsigned long long)((bytes + (1u << 20) - 1) >> 20));
}

}  // namespace

std::string video_size_failure(std::uint64_t size, const VideoLimits &limits,
                               bool over_modem)
{
    const std::uint64_t limit =
        over_modem ? limits.modem_bytes : limits.wifi_bytes;
    if (limit == 0 || size <= limit) {
        return "";
    }
    if (over_modem) {
        return "the video is too large for the cellular uplink (" +
               megabytes(size) + " MB, at most " + megabytes(limit) + " MB)";
    }
    return "the video is too large (" + megabytes(size) + " MB, at most " +
           megabytes(limit) + " MB)";
}

}  // namespace uplink
