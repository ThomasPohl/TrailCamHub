#pragma once

// The MP4 of a recording, built while it is read from the stored H.264 and
// AAC streams (no copy is written). The backend and WebDAV uploads share it;
// it has no ESP-IDF dependency, so the host tests cover it.

#include <string>

#include "mp4_file_source.hpp"
#include "mp4_mux.hpp"

namespace uplink
{

enum class Mp4Status {
    ok,
    video_missing,  // there is no H.264 file
    cannot_build,   // the streams cannot be turned into an MP4
};

struct Mp4Body {
    mp4::FileSource video;
    mp4::FileSource audio;
    mp4::Mp4File file;

    Mp4Body(const std::string &video_path, const std::string &audio_path);

    // Scans the streams. An audio file that is missing or empty is left out.
    Mp4Status open();

    // Why open() failed, for the log.
    std::string failure(Mp4Status status) const;
};

}  // namespace uplink
