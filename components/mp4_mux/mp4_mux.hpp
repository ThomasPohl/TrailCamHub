#pragma once

// Builds a classic MP4 file (ftyp, moov, mdat) from the raw streams that a
// video download leaves on the SD card: H.264 in Annex-B form and AAC with
// ADTS headers. The file is virtual: open() scans both streams once and
// builds the sample tables, read() then produces any byte range of the MP4 on
// demand, so a web server can send it (with a Content-Length and Range
// requests) without a stored copy. Pure logic, host-testable; the stream
// access is a small interface.
//
// The timing comes from the caller, as the raw streams hold none: a total
// duration spreads the video frames evenly; without one the duration of the
// audio is used, or 25 frames per second if there is no audio.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp4
{

// A readable byte stream of known size.
class Source
{
  public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const = 0;
    // Reads exactly `length` bytes at `offset`; false if that fails or runs
    // past the end.
    virtual bool read(std::uint64_t offset, std::uint8_t *destination,
                      std::size_t length) = 0;
};

struct Options {
    // Duration of the video in ms (the camera's totalTime); 0: unknown.
    std::uint32_t video_duration_ms = 0;
};

class Mp4File
{
  public:
    // Scans the streams and builds the layout. `audio` may be null or empty.
    // The sources must stay valid, and unchanged, while the file is used.
    bool open(Source &video, Source *audio, const Options &options = {});

    // Size of the whole MP4 file; valid after a successful open().
    std::uint64_t size() const
    {
        return m_size;
    }

    // Copies up to `length` bytes of the MP4 from `position`; returns the
    // number of bytes (less than `length` only at the end of the file), or
    // -1 if a stream cannot be read.
    std::int64_t read(std::uint64_t position, std::uint8_t *destination,
                      std::size_t length);

    const std::string &error() const
    {
        return m_error;
    }
    std::uint32_t video_frames() const;
    std::uint32_t audio_frames() const;
    std::uint32_t duration_ms() const
    {
        return m_duration_ms;
    }
    std::uint32_t width() const
    {
        return m_width;
    }
    std::uint32_t height() const
    {
        return m_height;
    }
    // Number of video frames before the first key frame, which cannot be
    // decoded and are left out.
    std::uint32_t skipped_frames() const
    {
        return m_skipped;
    }

  private:
    struct VideoSample {
        std::uint32_t source_begin;  // first byte of the access unit
        std::uint32_t source_length;
        std::uint32_t size;  // length in the MP4 (length-prefixed NALs)
        bool key;
    };
    struct AudioSample {
        std::uint32_t source_begin;  // first byte after the ADTS header
        std::uint32_t size;
    };
    struct Chunk {
        std::uint64_t start;  // offset in the mdat data
        std::uint32_t first;
        std::uint32_t count;
        bool video;
    };

    bool scan_video();
    bool scan_audio();
    bool fail(const char *message);
    void plan_chunks();
    void build_header(std::vector<std::uint8_t> &out,
                      std::uint64_t data_offset) const;
    bool load_video_sample(std::uint32_t index);
    std::int64_t read_data(std::uint64_t offset, std::uint8_t *destination,
                           std::size_t length);

    Source *m_video = nullptr;
    Source *m_audio = nullptr;
    Options m_options;
    std::string m_error;

    std::vector<VideoSample> m_video_samples;
    std::vector<AudioSample> m_audio_samples;
    std::vector<std::uint8_t> m_sps;
    std::vector<std::uint8_t> m_pps;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint8_t m_chroma_format = 1;
    std::uint8_t m_bit_depth_luma = 8;
    std::uint8_t m_bit_depth_chroma = 8;
    std::uint32_t m_skipped = 0;

    std::uint8_t m_audio_object_type = 2;
    std::uint8_t m_audio_rate_index = 8;
    std::uint8_t m_audio_channels = 1;
    std::uint32_t m_audio_rate = 16000;

    std::uint32_t m_duration_ms = 0;
    std::uint32_t m_video_delta = 0;  // in 1/90000 s
    std::vector<Chunk> m_chunks;
    std::uint64_t m_data_size = 0;
    std::vector<std::uint8_t> m_header;
    std::uint64_t m_size = 0;

    // The converted video sample that was read last, and the buffer that the
    // source bytes are read into.
    std::vector<std::uint8_t> m_sample;
    std::vector<std::uint8_t> m_raw;
    std::int64_t m_sample_index = -1;
};

// Fields of a sequence parameter set (NAL unit including its header byte).
struct SpsInfo {
    std::uint8_t profile = 0;
    std::uint8_t compatibility = 0;
    std::uint8_t level = 0;
    std::uint8_t chroma_format = 1;
    std::uint8_t bit_depth_luma = 8;
    std::uint8_t bit_depth_chroma = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
bool parse_sps(const std::uint8_t *nal, std::size_t length, SpsInfo &info);

}  // namespace mp4
