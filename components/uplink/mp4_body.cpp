#include "mp4_body.hpp"

namespace uplink
{

Mp4Body::Mp4Body(const std::string &video_path, const std::string &audio_path)
    : video(video_path), audio(audio_path)
{
}

Mp4Status Mp4Body::open()
{
    if (!video.exists()) {
        return Mp4Status::video_missing;
    }
    if (!file.open(video,
                   audio.exists() && audio.size() > 0 ? &audio : nullptr)) {
        return Mp4Status::cannot_build;
    }
    return Mp4Status::ok;
}

std::string Mp4Body::failure(Mp4Status status) const
{
    switch (status) {
    case Mp4Status::ok:
        return "";
    case Mp4Status::video_missing:
        return "the video is not on the SD card";
    case Mp4Status::cannot_build:
        break;
    }
    return "the MP4 cannot be built: " + file.error();
}

}  // namespace uplink
