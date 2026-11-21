#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mp4_body.hpp"

namespace
{

using Bytes = std::vector<std::uint8_t>;

// The SPS and PPS of a 1920x1080 High profile stream of the camera.
const Bytes kSps = {0x67, 0x64, 0x00, 0x28, 0xac, 0x3b, 0x50, 0x3c,
                    0x01, 0x13, 0xf2, 0xc2, 0x00, 0x00, 0x03, 0x00,
                    0x02, 0x00, 0x00, 0x03, 0x00, 0x79, 0x08};
const Bytes kPps = {0x68, 0xee, 0x3c, 0xe1, 0x00, 0x42, 0x42, 0x00, 0x84,
                    0x84, 0x04, 0x4c, 0x52, 0x1b, 0x93, 0xc5, 0x7c, 0x9f,
                    0x93, 0xf9, 0x3f, 0x27, 0xc9, 0xe6, 0xe4, 0xc9, 0x24,
                    0x2c, 0x22, 0x42, 0x90, 0x9c, 0x9e, 0x4f, 0xaf, 0xc9,
                    0xfd, 0x7e, 0x4f, 0xaf, 0x27, 0x26, 0xa4, 0xc0};

void append(Bytes &to, const Bytes &from)
{
    to.insert(to.end(), from.begin(), from.end());
}

// One key frame with parameter sets, then a few P frames.
Bytes make_video()
{
    const Bytes start = {0, 0, 0, 1};
    Bytes video;
    append(video, start);
    append(video, kSps);
    append(video, start);
    append(video, kPps);
    for (unsigned i = 0; i < 4; i++) {
        append(video, start);
        Bytes nal = {static_cast<std::uint8_t>(i == 0 ? 0x65 : 0x41), 0x88};
        for (unsigned k = 0; k < 60; k++) {
            nal.push_back(static_cast<std::uint8_t>(0x20 + (i * 7 + k) % 180));
        }
        append(video, nal);
    }
    return video;
}

void write_file(const std::string &path, const Bytes &data)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (!data.empty()) {
        REQUIRE(std::fwrite(data.data(), 1, data.size(), f) == data.size());
    }
    std::fclose(f);
}

}  // namespace

TEST_CASE("mp4 body: a missing video is reported as missing")
{
    uplink::Mp4Body body("no-such-video.h264", "no-such-audio.aac");
    const uplink::Mp4Status status = body.open();
    CHECK(status == uplink::Mp4Status::video_missing);
    CHECK(body.failure(status) == "the video is not on the SD card");
}

TEST_CASE("mp4 body: a stream without frames cannot be built")
{
    const std::string video = "test_mp4_body_bad.h264";
    write_file(video, Bytes{1, 2, 3});
    uplink::Mp4Body body(video, "no-such-audio.aac");
    const uplink::Mp4Status status = body.open();
    CHECK(status == uplink::Mp4Status::cannot_build);
    CHECK(body.failure(status).rfind("the MP4 cannot be built: ", 0) == 0);
    std::remove(video.c_str());
}

TEST_CASE("mp4 body: video without audio, and with an empty audio file")
{
    const std::string video = "test_mp4_body_ok.h264";
    const std::string audio = "test_mp4_body_ok.aac";
    write_file(video, make_video());

    uplink::Mp4Body without("test_mp4_body_ok.h264", "no-such-audio.aac");
    REQUIRE(without.open() == uplink::Mp4Status::ok);
    CHECK(without.file.size() > 0);
    CHECK(without.file.audio_frames() == 0);
    CHECK(without.failure(uplink::Mp4Status::ok).empty());
    Bytes head(8);
    REQUIRE(without.file.read(0, head.data(), head.size()) == 8);
    CHECK(std::string(head.begin() + 4, head.end()) == "ftyp");

    write_file(audio, Bytes{});
    uplink::Mp4Body empty(video, audio);
    REQUIRE(empty.open() == uplink::Mp4Status::ok);
    CHECK(empty.file.size() == without.file.size());

    std::remove(video.c_str());
    std::remove(audio.c_str());
}
