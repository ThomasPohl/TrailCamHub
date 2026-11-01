#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mp4_mux.hpp"

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

class VectorSource : public mp4::Source
{
  public:
    explicit VectorSource(Bytes data) : m_data(std::move(data))
    {
    }
    std::uint64_t size() const override
    {
        return m_data.size();
    }
    bool read(std::uint64_t offset, std::uint8_t *destination,
              std::size_t length) override
    {
        if (offset + length > m_data.size()) {
            return false;
        }
        std::memcpy(destination, &m_data[offset], length);
        return true;
    }

  private:
    Bytes m_data;
};

void append(Bytes &out, const Bytes &data)
{
    out.insert(out.end(), data.begin(), data.end());
}

// A slice NAL unit without start code: the header, the first byte with
// first_mb_in_slice 0 and a payload that cannot look like a start code.
Bytes slice(bool key, unsigned index, unsigned length)
{
    Bytes nal = {static_cast<std::uint8_t>(key ? 0x65 : 0x41), 0x88};
    for (unsigned i = 0; i < length; i++) {
        nal.push_back(static_cast<std::uint8_t>(0x20 + (index * 7 + i) % 180));
    }
    return nal;
}

struct Stream {
    Bytes video;
    Bytes audio;
    std::vector<Bytes> samples;  // slice NAL unit of every kept frame
    std::vector<Bytes> aac;      // raw AAC frames
};

// `lead` P frames, then `count` frames with a key frame every `gop`.
Stream make_stream(unsigned lead, unsigned count, unsigned gop,
                   unsigned audio_frames)
{
    Stream s;
    const Bytes start = {0, 0, 0, 1};
    const Bytes short_start = {0, 0, 1};
    for (unsigned i = 0; i < lead; i++) {
        append(s.video, start);
        append(s.video, slice(false, 900 + i, 50));
    }
    for (unsigned i = 0; i < count; i++) {
        const bool key = i % gop == 0;
        if (key) {
            append(s.video, start);
            append(s.video, kSps);
            append(s.video, start);
            append(s.video, kPps);
        }
        // Mix 4-byte and 3-byte start codes.
        append(s.video, (i % 3 == 1) ? short_start : start);
        const Bytes nal = slice(key, i, 40 + (i * 13) % 300);
        append(s.video, nal);
        s.samples.push_back(nal);
    }
    for (unsigned i = 0; i < audio_frames; i++) {
        Bytes payload;
        for (unsigned k = 0; k < 20 + (i * 5) % 60; k++) {
            payload.push_back(static_cast<std::uint8_t>(i + k));
        }
        const unsigned length = 7 + payload.size();
        // AAC-LC, 16 kHz (index 8), mono, no CRC.
        const Bytes header = {
            0xff,
            0xf1,
            static_cast<std::uint8_t>((1 << 6) | (8 << 2)),
            static_cast<std::uint8_t>((1 << 6) | (length >> 11)),
            static_cast<std::uint8_t>((length >> 3) & 0xff),
            static_cast<std::uint8_t>(((length & 7) << 5) | 0x1f),
            0xfc};
        append(s.audio, header);
        append(s.audio, payload);
        s.aac.push_back(payload);
    }
    return s;
}

std::uint32_t be32(const Bytes &d, std::size_t at)
{
    return (std::uint32_t(d[at]) << 24) | (std::uint32_t(d[at + 1]) << 16) |
           (std::uint32_t(d[at + 2]) << 8) | d[at + 3];
}

// Offset of the first box of `type` among the boxes in [begin, end), or
// npos.
std::size_t find_box(const Bytes &d, std::size_t begin, std::size_t end,
                     const char *type)
{
    while (begin + 8 <= end) {
        const std::size_t size = be32(d, begin);
        if (size < 8 || begin + size > end) {
            return std::string::npos;
        }
        if (std::memcmp(&d[begin + 4], type, 4) == 0) {
            return begin;
        }
        begin += size;
    }
    return std::string::npos;
}

std::size_t find_path(const Bytes &d, std::size_t begin, std::size_t end,
                      std::initializer_list<const char *> path)
{
    std::size_t at = std::string::npos;
    for (const char *type : path) {
        at = find_box(d, begin, end, type);
        if (at == std::string::npos) {
            return at;
        }
        begin = at + 8;
        end = at + be32(d, at);
    }
    return at;
}

Bytes read_all(mp4::Mp4File &file, std::size_t step)
{
    Bytes out(static_cast<std::size_t>(file.size()));
    std::size_t position = 0;
    while (position < out.size()) {
        const std::int64_t n = file.read(position, &out[position],
                                         std::min(step, out.size() - position));
        REQUIRE(n > 0);
        position += static_cast<std::size_t>(n);
    }
    return out;
}

// The sample offsets of the track `index` (0: video) from its tables.
std::vector<std::pair<std::size_t, std::size_t>> sample_extents(const Bytes &d,
                                                                int index)
{
    const std::size_t moov = find_box(d, 0, d.size(), "moov");
    REQUIRE(moov != std::string::npos);
    std::size_t trak = find_box(d, moov + 8, moov + be32(d, moov), "trak");
    for (int i = 0; i < index; i++) {
        const std::size_t after = trak + be32(d, trak);
        trak = find_box(d, after, moov + be32(d, moov), "trak");
    }
    REQUIRE(trak != std::string::npos);
    const std::size_t stbl =
        find_path(d, trak + 8, trak + be32(d, trak), {"mdia", "minf", "stbl"});
    REQUIRE(stbl != std::string::npos);
    const std::size_t end = stbl + be32(d, stbl);
    const std::size_t stsz = find_box(d, stbl + 8, end, "stsz");
    const std::size_t stsc = find_box(d, stbl + 8, end, "stsc");
    const std::size_t stco = find_box(d, stbl + 8, end, "stco");
    REQUIRE(stsz != std::string::npos);
    REQUIRE(stsc != std::string::npos);
    REQUIRE(stco != std::string::npos);

    const std::uint32_t samples = be32(d, stsz + 16);
    const std::uint32_t chunks = be32(d, stco + 12);
    const std::uint32_t runs = be32(d, stsc + 12);
    std::vector<std::pair<std::size_t, std::size_t>> extents;
    std::uint32_t sample = 0;
    for (std::uint32_t chunk = 1; chunk <= chunks; chunk++) {
        std::uint32_t per_chunk = 0;
        for (std::uint32_t r = 0; r < runs; r++) {
            if (be32(d, stsc + 16 + r * 12) <= chunk) {
                per_chunk = be32(d, stsc + 16 + r * 12 + 4);
            }
        }
        std::size_t offset = be32(d, stco + 16 + (chunk - 1) * 4);
        for (std::uint32_t k = 0; k < per_chunk && sample < samples; k++) {
            const std::size_t size = be32(d, stsz + 20 + sample * 4);
            extents.push_back({offset, size});
            offset += size;
            sample++;
        }
    }
    REQUIRE(extents.size() == samples);
    return extents;
}

}  // namespace

TEST_CASE("mp4: the SPS of the camera gives 1920x1080")
{
    mp4::SpsInfo info;
    REQUIRE(mp4::parse_sps(kSps.data(), kSps.size(), info));
    CHECK(info.profile == 100);
    CHECK(info.level == 40);
    CHECK(info.chroma_format == 1);
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK_FALSE(mp4::parse_sps(kPps.data(), kPps.size(), info));
}

TEST_CASE("mp4: layout of the file and sample data")
{
    const Stream s = make_stream(0, 70, 25, 40);
    VectorSource video(s.video);
    VectorSource audio(s.audio);
    mp4::Mp4File file;
    REQUIRE(file.open(video, &audio));
    CHECK(file.video_frames() == 70);
    CHECK(file.audio_frames() == 40);
    CHECK(file.width() == 1920);
    CHECK(file.height() == 1080);

    const Bytes d = read_all(file, 100000);
    REQUIRE(d.size() == file.size());
    // ftyp, moov, mdat fill the file.
    const std::size_t ftyp = find_box(d, 0, d.size(), "ftyp");
    const std::size_t moov = find_box(d, 0, d.size(), "moov");
    const std::size_t mdat = find_box(d, 0, d.size(), "mdat");
    CHECK(ftyp == 0);
    CHECK(moov == be32(d, ftyp));
    CHECK(mdat == moov + be32(d, moov));
    CHECK(mdat + be32(d, mdat) == d.size());

    // Every video sample is the slice with a 4-byte length.
    const auto video_extents = sample_extents(d, 0);
    REQUIRE(video_extents.size() == 70);
    for (std::size_t i = 0; i < video_extents.size(); i++) {
        const auto [offset, size] = video_extents[i];
        REQUIRE(offset >= mdat + 8);
        REQUIRE(offset + size <= d.size());
        CHECK(size == s.samples[i].size() + 4);
        CHECK(be32(d, offset) == s.samples[i].size());
        CHECK(std::memcmp(&d[offset + 4], s.samples[i].data(),
                          s.samples[i].size()) == 0);
    }
    // Every audio sample is the payload without the ADTS header.
    const auto audio_extents = sample_extents(d, 1);
    REQUIRE(audio_extents.size() == 40);
    for (std::size_t i = 0; i < audio_extents.size(); i++) {
        const auto [offset, size] = audio_extents[i];
        CHECK(size == s.aac[i].size());
        CHECK(std::memcmp(&d[offset], s.aac[i].data(), size) == 0);
    }
    // The data of the samples tile the mdat box without gaps.
    std::size_t covered = 0;
    for (const auto &e : video_extents) {
        covered += e.second;
    }
    for (const auto &e : audio_extents) {
        covered += e.second;
    }
    CHECK(covered + 8 == be32(d, mdat));
}

TEST_CASE("mp4: reading in pieces gives the same bytes")
{
    const Stream s = make_stream(0, 70, 25, 40);
    VectorSource video(s.video);
    VectorSource audio(s.audio);
    mp4::Mp4File file;
    REQUIRE(file.open(video, &audio));
    const Bytes whole = read_all(file, 1 << 20);
    for (std::size_t step : {1u, 7u, 100u, 4093u, 65536u}) {
        mp4::Mp4File other;
        REQUIRE(other.open(video, &audio));
        const Bytes pieces = read_all(other, step);
        CHECK(pieces == whole);
    }
    // Ranges in the middle, backwards, and across the end.
    Bytes part(500);
    for (std::uint64_t position : {whole.size() - 500, std::size_t(3000),
                                   std::size_t(0), whole.size() / 2}) {
        REQUIRE(file.read(position, part.data(), part.size()) == 500);
        CHECK(std::memcmp(part.data(), &whole[position], 500) == 0);
    }
    CHECK(file.read(whole.size() - 10, part.data(), part.size()) == 10);
    CHECK(file.read(whole.size(), part.data(), part.size()) == 0);
    CHECK(file.read(whole.size() + 100, part.data(), part.size()) == 0);
}

TEST_CASE("mp4: frames before the first key frame are left out")
{
    const Stream s = make_stream(2, 30, 25, 20);
    VectorSource video(s.video);
    VectorSource audio(s.audio);
    mp4::Mp4File file;
    REQUIRE(file.open(video, &audio));
    CHECK(file.skipped_frames() == 2);
    CHECK(file.video_frames() == 30);
    const Bytes d = read_all(file, 4096);
    const auto extents = sample_extents(d, 0);
    REQUIRE(extents.size() == 30);
    CHECK(std::memcmp(&d[extents[0].first + 4], s.samples[0].data(),
                      s.samples[0].size()) == 0);
}

TEST_CASE("mp4: duration from the caller, from the audio, or 25 fps")
{
    const Stream s = make_stream(0, 50, 25, 30);
    VectorSource video(s.video);
    VectorSource audio(s.audio);
    {
        mp4::Mp4File file;
        mp4::Options options;
        options.video_duration_ms = 2000;
        REQUIRE(file.open(video, &audio, options));
        CHECK(file.duration_ms() == 2000);
        const Bytes d = read_all(file, 4096);
        const std::size_t mvhd = find_path(d, 0, d.size(), {"moov", "mvhd"});
        CHECK(be32(d, mvhd + 20) == 1000);  // timescale
        CHECK(be32(d, mvhd + 24) == 2000);  // duration
    }
    {
        // 30 audio frames of 1024 samples at 16 kHz: 1920 ms.
        mp4::Mp4File file;
        REQUIRE(file.open(video, &audio));
        CHECK(file.duration_ms() == 1920);
    }
    {
        mp4::Mp4File file;
        REQUIRE(file.open(video, nullptr));
        CHECK(file.duration_ms() == 2000);  // 50 frames at 25 fps
        CHECK(file.audio_frames() == 0);
        const Bytes d = read_all(file, 4096);
        const std::size_t moov = find_box(d, 0, d.size(), "moov");
        const std::size_t first =
            find_box(d, moov + 8, moov + be32(d, moov), "trak");
        const std::size_t after = first + be32(d, first);
        CHECK(find_box(d, after, moov + be32(d, moov), "trak") ==
              std::string::npos);
    }
}

TEST_CASE("mp4: the audio is found after garbage and parameters are kept")
{
    Stream s = make_stream(0, 30, 25, 10);
    Bytes noisy = {0x12, 0xff, 0x00, 0x34, 0xff, 0xf1};
    append(noisy, s.audio);
    VectorSource video(s.video);
    VectorSource audio(noisy);
    mp4::Mp4File file;
    REQUIRE(file.open(video, &audio));
    CHECK(file.audio_frames() == 10);
    const Bytes d = read_all(file, 4096);
    const std::size_t esds = [&] {
        for (std::size_t i = 0; i + 4 < d.size(); i++) {
            if (std::memcmp(&d[i], "esds", 4) == 0) {
                return i - 4;
            }
        }
        return std::string::npos;
    }();
    REQUIRE(esds != std::string::npos);
    // AudioSpecificConfig: AAC-LC (2), 16 kHz (8), mono (1): 0x1408.
    bool found = false;
    for (std::size_t i = esds; i + 3 < esds + be32(d, esds); i++) {
        if (d[i] == 0x05 && d[i + 1] == 0x02 && d[i + 2] == 0x14 &&
            d[i + 3] == 0x08) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("mp4: streams that cannot be turned into a file are refused")
{
    {
        VectorSource video(Bytes{});
        mp4::Mp4File file;
        CHECK_FALSE(file.open(video, nullptr));
        CHECK_FALSE(file.error().empty());
    }
    {
        // Only P frames: nothing to start from.
        const Stream s = make_stream(5, 0, 25, 0);
        VectorSource video(s.video);
        mp4::Mp4File file;
        CHECK_FALSE(file.open(video, nullptr));
    }
    {
        // A key frame without parameter sets.
        Bytes data = {0, 0, 0, 1};
        append(data, slice(true, 0, 40));
        VectorSource video(data);
        mp4::Mp4File file;
        CHECK_FALSE(file.open(video, nullptr));
    }
}
