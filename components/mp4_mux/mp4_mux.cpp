#include "mp4_mux.hpp"

#include <algorithm>
#include <cstring>

namespace mp4
{

namespace
{

constexpr std::uint32_t kVideoTimescale = 90000;
constexpr std::uint32_t kChunkMs = 1000;
constexpr std::size_t kScanBlock = 8192;
constexpr std::uint64_t kMaxStream = 0xF0000000ull;
constexpr std::uint32_t kAacSamples = 1024;

const std::uint32_t kAacRates[13] = {96000, 88200, 64000, 48000, 44100,
                                     32000, 24000, 22050, 16000, 12000,
                                     11025, 8000,  7350};

// Writes big-endian fields and boxes whose size is patched in at the end.
class Writer
{
  public:
    explicit Writer(std::vector<std::uint8_t> &out) : m_out(out)
    {
    }

    void u8(std::uint32_t v)
    {
        m_out.push_back(static_cast<std::uint8_t>(v));
    }
    void u16(std::uint32_t v)
    {
        u8(v >> 8);
        u8(v);
    }
    void u32(std::uint32_t v)
    {
        u16(v >> 16);
        u16(v);
    }
    void bytes(const void *data, std::size_t length)
    {
        const auto *p = static_cast<const std::uint8_t *>(data);
        m_out.insert(m_out.end(), p, p + length);
    }
    void zeros(std::size_t length)
    {
        m_out.insert(m_out.end(), length, 0);
    }
    void text(const char *s)
    {
        bytes(s, std::strlen(s));
    }

    std::size_t begin(const char *type)
    {
        const std::size_t at = m_out.size();
        u32(0);
        bytes(type, 4);
        return at;
    }
    std::size_t begin_full(const char *type, std::uint32_t version,
                           std::uint32_t flags)
    {
        const std::size_t at = begin(type);
        u32((version << 24) | flags);
        return at;
    }
    void end(std::size_t at)
    {
        const std::uint32_t size =
            static_cast<std::uint32_t>(m_out.size() - at);
        m_out[at] = static_cast<std::uint8_t>(size >> 24);
        m_out[at + 1] = static_cast<std::uint8_t>(size >> 16);
        m_out[at + 2] = static_cast<std::uint8_t>(size >> 8);
        m_out[at + 3] = static_cast<std::uint8_t>(size);
    }

  private:
    std::vector<std::uint8_t> &m_out;
};

class Bits
{
  public:
    Bits(const std::uint8_t *data, std::size_t length)
        : m_data(data), m_length(length)
    {
    }
    bool ok() const
    {
        return m_ok;
    }
    std::uint32_t bit()
    {
        if (m_position >= m_length * 8) {
            m_ok = false;
            return 0;
        }
        const std::uint32_t v =
            (m_data[m_position >> 3] >> (7 - (m_position & 7))) & 1;
        m_position++;
        return v;
    }
    std::uint32_t bits(unsigned count)
    {
        std::uint32_t v = 0;
        while (count-- > 0) {
            v = (v << 1) | bit();
        }
        return v;
    }
    std::uint32_t ue()
    {
        unsigned zeros = 0;
        while (bit() == 0 && m_ok) {
            if (++zeros > 31) {
                m_ok = false;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }
    std::int32_t se()
    {
        const std::uint32_t v = ue();
        return (v & 1) ? static_cast<std::int32_t>((v + 1) >> 1)
                       : -static_cast<std::int32_t>(v >> 1);
    }

  private:
    const std::uint8_t *m_data;
    std::size_t m_length;
    std::size_t m_position = 0;
    bool m_ok = true;
};

void skip_scaling_list(Bits &bits, unsigned size)
{
    std::int32_t last = 8;
    std::int32_t next = 8;
    for (unsigned i = 0; i < size; i++) {
        if (next != 0) {
            next = (last + bits.se() + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
}

}  // namespace

bool parse_sps(const std::uint8_t *nal, std::size_t length, SpsInfo &info)
{
    if (length < 5 || (nal[0] & 0x1f) != 7) {
        return false;
    }
    // Remove the emulation prevention bytes.
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(length);
    for (std::size_t i = 1; i < length; i++) {
        const std::size_t n = rbsp.size();
        if (n >= 2 && rbsp[n - 1] == 0 && rbsp[n - 2] == 0 && nal[i] == 3) {
            continue;
        }
        rbsp.push_back(nal[i]);
    }
    Bits bits(rbsp.data(), rbsp.size());
    info.profile = static_cast<std::uint8_t>(bits.bits(8));
    info.compatibility = static_cast<std::uint8_t>(bits.bits(8));
    info.level = static_cast<std::uint8_t>(bits.bits(8));
    bits.ue();  // seq_parameter_set_id
    info.chroma_format = 1;
    info.bit_depth_luma = 8;
    info.bit_depth_chroma = 8;
    switch (info.profile) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135: {
        info.chroma_format = static_cast<std::uint8_t>(bits.ue());
        if (info.chroma_format == 3) {
            bits.bit();  // separate_colour_plane_flag
        }
        info.bit_depth_luma = static_cast<std::uint8_t>(bits.ue() + 8);
        info.bit_depth_chroma = static_cast<std::uint8_t>(bits.ue() + 8);
        bits.bit();  // qpprime_y_zero_transform_bypass_flag
        if (bits.bit()) {
            const unsigned lists = info.chroma_format != 3 ? 8 : 12;
            for (unsigned i = 0; i < lists; i++) {
                if (bits.bit()) {
                    skip_scaling_list(bits, i < 6 ? 16 : 64);
                }
            }
        }
        break;
    }
    default:
        break;
    }
    bits.ue();  // log2_max_frame_num_minus4
    const std::uint32_t poc_type = bits.ue();
    if (poc_type == 0) {
        bits.ue();
    } else if (poc_type == 1) {
        bits.bit();
        bits.se();
        bits.se();
        const std::uint32_t cycle = bits.ue();
        for (std::uint32_t i = 0; i < cycle && bits.ok(); i++) {
            bits.se();
        }
    }
    bits.ue();   // max_num_ref_frames
    bits.bit();  // gaps_in_frame_num_value_allowed_flag
    const std::uint32_t width_mbs = bits.ue() + 1;
    const std::uint32_t height_units = bits.ue() + 1;
    const std::uint32_t frame_mbs_only = bits.bit();
    if (!frame_mbs_only) {
        bits.bit();  // mb_adaptive_frame_field_flag
    }
    bits.bit();  // direct_8x8_inference_flag
    std::uint32_t crop_left = 0;
    std::uint32_t crop_right = 0;
    std::uint32_t crop_top = 0;
    std::uint32_t crop_bottom = 0;
    if (bits.bit()) {
        crop_left = bits.ue();
        crop_right = bits.ue();
        crop_top = bits.ue();
        crop_bottom = bits.ue();
    }
    if (!bits.ok() || width_mbs == 0 || width_mbs > 1024 ||
        height_units > 1024) {
        return false;
    }
    const std::uint32_t unit_x = info.chroma_format == 0 ? 1 : 2;
    const std::uint32_t unit_y =
        (info.chroma_format == 1 ? 2 : 1) * (frame_mbs_only ? 1 : 2);
    if (info.chroma_format == 0) {
        // Monochrome: crop units are 1 (times 2 for field coding).
    }
    const std::uint32_t full_width = width_mbs * 16;
    const std::uint32_t full_height =
        (frame_mbs_only ? 1 : 2) * height_units * 16;
    const std::uint32_t cut_x = (crop_left + crop_right) * unit_x;
    const std::uint32_t cut_y = (crop_top + crop_bottom) * unit_y;
    if (cut_x >= full_width || cut_y >= full_height) {
        return false;
    }
    info.width = full_width - cut_x;
    info.height = full_height - cut_y;
    return true;
}

bool Mp4File::fail(const char *message)
{
    m_error = message;
    return false;
}

std::uint32_t Mp4File::video_frames() const
{
    return static_cast<std::uint32_t>(m_video_samples.size());
}

std::uint32_t Mp4File::audio_frames() const
{
    return static_cast<std::uint32_t>(m_audio_samples.size());
}

bool Mp4File::scan_video()
{
    const std::uint64_t total = m_video->size();
    if (total == 0) {
        return fail("the video stream is empty");
    }
    if (total > kMaxStream) {
        return fail("the video stream is too large");
    }

    // The access units: a unit starts at a parameter set, SEI or delimiter
    // that follows a slice, or at a slice that starts at macroblock 0. Units
    // before the first key frame cannot be decoded and are skipped. The
    // sample of a unit holds its slices, each with a 4-byte length.
    std::vector<std::uint8_t> block(kScanBlock);
    enum { kSearch, kHeader, kSliceByte } state = kSearch;
    unsigned zeros = 0;
    std::uint64_t sc_begin = 0;  // of the NAL unit being read
    std::uint64_t nal_begin = 0;
    int nal_type = -1;
    bool in_nal = false;
    bool capturing = false;
    std::vector<std::uint8_t> capture;
    bool au_open = false;
    bool au_slice = false;
    bool au_key = false;
    std::uint64_t au_begin = 0;
    std::uint64_t au_size = 0;
    bool key_seen = false;

    auto start_au = [&](std::uint64_t at) {
        au_open = true;
        au_begin = at;
        au_slice = false;
        au_key = false;
        au_size = 0;
    };
    auto flush_au = [&](std::uint64_t end) {
        if (au_open && au_slice) {
            if (au_key) {
                key_seen = true;
            }
            if (key_seen) {
                m_video_samples.push_back(
                    {static_cast<std::uint32_t>(au_begin),
                     static_cast<std::uint32_t>(end - au_begin),
                     static_cast<std::uint32_t>(au_size), au_key});
            } else {
                m_skipped++;
            }
        }
        au_open = false;
        au_slice = false;
        au_key = false;
        au_size = 0;
    };
    auto finish_nal = [&](std::uint64_t end) {
        if (!in_nal) {
            return;
        }
        if (nal_type == 1 || nal_type == 5) {
            au_size += 4 + (end - nal_begin);
        }
        if (capturing) {
            while (!capture.empty() && capture.back() == 0) {
                capture.pop_back();
            }
            if (nal_type == 7 && m_sps.empty()) {
                m_sps = capture;
            } else if (nal_type == 8 && m_pps.empty()) {
                m_pps = capture;
            }
            capturing = false;
        }
        in_nal = false;
    };

    for (std::uint64_t position = 0; position < total;) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(kScanBlock, total - position));
        if (!m_video->read(position, block.data(), count)) {
            return fail("cannot read the video stream");
        }
        for (std::size_t i = 0; i < count; i++) {
            const std::uint8_t b = block[i];
            const std::uint64_t p = position + i;
            if (state == kHeader) {
                nal_type = b & 0x1f;
                in_nal = true;
                state = kSearch;
                zeros = 0;
                if (nal_type >= 6 && nal_type <= 9) {
                    if (au_open && au_slice) {
                        flush_au(sc_begin);
                    }
                    if (!au_open) {
                        start_au(sc_begin);
                    }
                } else if (nal_type == 1 || nal_type == 5) {
                    if (!au_open) {
                        start_au(sc_begin);
                    }
                    state = kSliceByte;
                }
                if ((nal_type == 7 && m_sps.empty()) ||
                    (nal_type == 8 && m_pps.empty())) {
                    capturing = true;
                    capture.clear();
                    capture.push_back(b);
                }
                continue;
            }
            if (state == kSliceByte) {
                state = kSearch;
                if ((b & 0x80) && au_slice) {  // first_mb_in_slice is 0
                    flush_au(sc_begin);
                    start_au(sc_begin);
                }
                au_slice = true;
                if (nal_type == 5) {
                    au_key = true;
                }
            }
            if (b == 1 && zeros >= 2) {
                const std::uint64_t begin = p - std::min(zeros, 3u);
                finish_nal(begin);
                sc_begin = begin;
                nal_begin = p + 1;
                state = kHeader;
                zeros = 0;
                continue;
            }
            if (capturing && capture.size() < 256) {
                capture.push_back(b);
            }
            zeros = b == 0 ? zeros + 1 : 0;
        }
        position += count;
    }
    finish_nal(total);
    flush_au(total);

    if (m_video_samples.empty()) {
        return fail("the video stream has no key frame");
    }
    if (m_sps.empty() || m_pps.empty()) {
        return fail("the video stream has no SPS or PPS");
    }
    SpsInfo info;
    if (!parse_sps(m_sps.data(), m_sps.size(), info)) {
        return fail("cannot read the SPS");
    }
    m_width = info.width;
    m_height = info.height;
    m_chroma_format = info.chroma_format;
    m_bit_depth_luma = info.bit_depth_luma;
    m_bit_depth_chroma = info.bit_depth_chroma;
    return true;
}

bool Mp4File::scan_audio()
{
    if (m_audio == nullptr || m_audio->size() == 0) {
        return true;
    }
    const std::uint64_t total = m_audio->size();
    if (total > kMaxStream) {
        return fail("the audio stream is too large");
    }
    std::vector<std::uint8_t> cache(4096);
    std::uint64_t cache_start = 0;
    std::size_t cache_length = 0;
    auto fetch = [&](std::uint64_t offset, std::uint8_t *destination,
                     std::size_t length) {
        for (std::size_t i = 0; i < length; i++) {
            const std::uint64_t at = offset + i;
            if (at < cache_start || at >= cache_start + cache_length) {
                cache_start = at;
                cache_length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(cache.size(), total - at));
                if (!m_audio->read(cache_start, cache.data(), cache_length)) {
                    cache_length = 0;
                    return false;
                }
            }
            destination[i] = cache[static_cast<std::size_t>(at - cache_start)];
        }
        return true;
    };

    bool first = true;
    std::uint64_t position = 0;
    while (position + 7 <= total) {
        std::uint8_t h[9] = {};
        const std::size_t have = static_cast<std::size_t>(
            std::min<std::uint64_t>(sizeof(h), total - position));
        if (!fetch(position, h, have)) {
            return fail("cannot read the audio stream");
        }
        bool valid = h[0] == 0xff && (h[1] & 0xf6) == 0xf0;
        std::uint32_t rate_index = 0;
        std::uint32_t channels = 0;
        std::uint32_t frame_length = 0;
        std::uint32_t header_length = 0;
        if (valid) {
            header_length = (h[1] & 1) ? 7 : 9;
            rate_index = (h[2] >> 2) & 0xf;
            channels = ((h[2] & 1) << 2) | (h[3] >> 6);
            frame_length = (std::uint32_t(h[3] & 3) << 11) |
                           (std::uint32_t(h[4]) << 3) | (h[5] >> 5);
            valid = rate_index < 13 && channels >= 1 &&
                    frame_length > header_length &&
                    position + frame_length <= total && have >= header_length;
        }
        if (!valid) {
            position++;  // look for the next frame
            continue;
        }
        if (first) {
            m_audio_object_type =
                static_cast<std::uint8_t>(((h[2] >> 6) & 3) + 1);
            m_audio_rate_index = static_cast<std::uint8_t>(rate_index);
            m_audio_channels = static_cast<std::uint8_t>(channels);
            m_audio_rate = kAacRates[rate_index];
            first = false;
        }
        m_audio_samples.push_back(
            {static_cast<std::uint32_t>(position + header_length),
             frame_length - header_length});
        position += frame_length;
    }
    return true;
}

void Mp4File::plan_chunks()
{
    // Audio and video are interleaved in chunks of one second.
    std::uint64_t offset = 0;
    std::uint32_t v = 0;
    std::uint32_t a = 0;
    const std::uint32_t nv = video_frames();
    const std::uint32_t na = audio_frames();
    for (std::uint64_t second = 0; v < nv || a < na; second++) {
        const std::uint64_t limit_ms = (second + 1) * kChunkMs;
        std::uint32_t end = v;
        std::uint64_t bytes = 0;
        while (end < nv &&
               static_cast<std::uint64_t>(end) * m_video_delta / 90 <
                   limit_ms) {
            bytes += m_video_samples[end].size;
            end++;
        }
        if (end > v) {
            m_chunks.push_back({offset, v, end - v, true});
            offset += bytes;
            v = end;
        }
        end = a;
        bytes = 0;
        while (end < na && static_cast<std::uint64_t>(end) * kAacSamples *
                                   1000 / m_audio_rate <
                               limit_ms) {
            bytes += m_audio_samples[end].size;
            end++;
        }
        if (end > a) {
            m_chunks.push_back({offset, a, end - a, false});
            offset += bytes;
            a = end;
        }
    }
    m_data_size = offset;
}

void Mp4File::build_header(std::vector<std::uint8_t> &out,
                           std::uint64_t data_offset) const
{
    Writer w(out);
    static const std::uint32_t kMatrix[9] = {
        0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    const bool has_audio = !m_audio_samples.empty();
    const std::uint32_t nv = video_frames();
    const std::uint32_t na = audio_frames();

    std::size_t at = w.begin("ftyp");
    w.text("isom");
    w.u32(0x200);
    w.text("isomiso2avc1mp41");
    w.end(at);

    const std::size_t moov = w.begin("moov");
    at = w.begin_full("mvhd", 0, 0);
    w.u32(0);
    w.u32(0);
    w.u32(1000);
    w.u32(m_duration_ms);
    w.u32(0x00010000);
    w.u16(0x0100);
    w.zeros(10);
    for (std::uint32_t m : kMatrix) {
        w.u32(m);
    }
    w.zeros(24);
    w.u32(has_audio ? 3 : 2);
    w.end(at);

    for (int track = 0; track < (has_audio ? 2 : 1); track++) {
        const bool video = track == 0;
        const std::uint32_t timescale = video ? kVideoTimescale : m_audio_rate;
        const std::uint64_t media_duration =
            video ? static_cast<std::uint64_t>(nv) * m_video_delta
                  : static_cast<std::uint64_t>(na) * kAacSamples;
        const std::uint32_t media_ms =
            static_cast<std::uint32_t>(media_duration * 1000 / timescale);
        const std::size_t trak = w.begin("trak");
        at = w.begin_full("tkhd", 0, 7);
        w.u32(0);
        w.u32(0);
        w.u32(track + 1);
        w.u32(0);
        w.u32(media_ms);
        w.zeros(8);
        w.u16(0);
        w.u16(0);
        w.u16(video ? 0 : 0x0100);
        w.u16(0);
        for (std::uint32_t m : kMatrix) {
            w.u32(m);
        }
        w.u32(video ? m_width << 16 : 0);
        w.u32(video ? m_height << 16 : 0);
        w.end(at);

        const std::size_t mdia = w.begin("mdia");
        at = w.begin_full("mdhd", 0, 0);
        w.u32(0);
        w.u32(0);
        w.u32(timescale);
        w.u32(static_cast<std::uint32_t>(media_duration));
        w.u16(0x55c4);
        w.u16(0);
        w.end(at);
        at = w.begin_full("hdlr", 0, 0);
        w.u32(0);
        w.text(video ? "vide" : "soun");
        w.zeros(12);
        w.text(video ? "VideoHandler" : "SoundHandler");
        w.u8(0);
        w.end(at);

        const std::size_t minf = w.begin("minf");
        if (video) {
            at = w.begin_full("vmhd", 0, 1);
            w.zeros(8);
            w.end(at);
        } else {
            at = w.begin_full("smhd", 0, 0);
            w.zeros(4);
            w.end(at);
        }
        at = w.begin("dinf");
        const std::size_t dref = w.begin_full("dref", 0, 0);
        w.u32(1);
        const std::size_t url = w.begin_full("url ", 0, 1);
        w.end(url);
        w.end(dref);
        w.end(at);

        const std::size_t stbl = w.begin("stbl");
        at = w.begin_full("stsd", 0, 0);
        w.u32(1);
        if (video) {
            const std::size_t avc1 = w.begin("avc1");
            w.zeros(6);
            w.u16(1);
            w.zeros(16);
            w.u16(m_width);
            w.u16(m_height);
            w.u32(0x00480000);
            w.u32(0x00480000);
            w.u32(0);
            w.u16(1);
            w.zeros(32);
            w.u16(0x0018);
            w.u16(0xffff);
            const std::size_t avcc = w.begin("avcC");
            w.u8(1);
            w.u8(m_sps.size() > 1 ? m_sps[1] : 0);
            w.u8(m_sps.size() > 2 ? m_sps[2] : 0);
            w.u8(m_sps.size() > 3 ? m_sps[3] : 0);
            w.u8(0xff);  // 4-byte NAL lengths
            w.u8(0xe1);
            w.u16(static_cast<std::uint32_t>(m_sps.size()));
            w.bytes(m_sps.data(), m_sps.size());
            w.u8(1);
            w.u16(static_cast<std::uint32_t>(m_pps.size()));
            w.bytes(m_pps.data(), m_pps.size());
            const std::uint8_t profile = m_sps.size() > 1 ? m_sps[1] : 0;
            if (profile == 100 || profile == 110 || profile == 122 ||
                profile == 144) {
                w.u8(0xfc | m_chroma_format);
                w.u8(0xf8 | (m_bit_depth_luma - 8));
                w.u8(0xf8 | (m_bit_depth_chroma - 8));
                w.u8(0);
            }
            w.end(avcc);
            w.end(avc1);
        } else {
            const std::size_t mp4a = w.begin("mp4a");
            w.zeros(6);
            w.u16(1);
            w.zeros(8);
            w.u16(m_audio_channels);
            w.u16(16);
            w.u32(0);
            w.u32(m_audio_rate << 16);
            const std::size_t esds = w.begin_full("esds", 0, 0);
            std::uint64_t audio_bytes = 0;
            for (const AudioSample &s : m_audio_samples) {
                audio_bytes += s.size;
            }
            const std::uint32_t bitrate =
                m_duration_ms == 0 ? 0
                                   : static_cast<std::uint32_t>(
                                         audio_bytes * 8000 / m_duration_ms);
            w.u8(0x03);  // ES_Descriptor
            w.u8(3 + 2 + 17 + 3);
            w.u16(0);
            w.u8(0);
            w.u8(0x04);  // DecoderConfigDescriptor
            w.u8(17);
            w.u8(0x40);  // MPEG-4 audio
            w.u8(0x15);  // audio stream
            w.zeros(3);
            w.u32(bitrate);
            w.u32(bitrate);
            w.u8(0x05);  // AudioSpecificConfig
            w.u8(2);
            w.u16((std::uint32_t(m_audio_object_type) << 11) |
                  (std::uint32_t(m_audio_rate_index) << 7) |
                  (std::uint32_t(m_audio_channels) << 3));
            w.u8(0x06);  // SLConfigDescriptor
            w.u8(1);
            w.u8(2);
            w.end(esds);
            w.end(mp4a);
        }
        w.end(at);

        // Sample timing: one entry, as every sample has the same length.
        const std::uint32_t samples = video ? nv : na;
        at = w.begin_full("stts", 0, 0);
        w.u32(1);
        w.u32(samples);
        w.u32(video ? m_video_delta : kAacSamples);
        w.end(at);
        if (video) {
            at = w.begin_full("stss", 0, 0);
            std::uint32_t keys = 0;
            for (const VideoSample &s : m_video_samples) {
                keys += s.key ? 1 : 0;
            }
            w.u32(keys);
            for (std::uint32_t i = 0; i < nv; i++) {
                if (m_video_samples[i].key) {
                    w.u32(i + 1);
                }
            }
            w.end(at);
        }
        // Samples per chunk, as runs of equal chunks.
        at = w.begin_full("stsc", 0, 0);
        std::size_t entries_at = out.size();
        w.u32(0);
        std::uint32_t entries = 0;
        std::uint32_t chunk_number = 0;
        std::uint32_t previous = 0;
        for (const Chunk &c : m_chunks) {
            if (c.video != video) {
                continue;
            }
            chunk_number++;
            if (c.count != previous) {
                w.u32(chunk_number);
                w.u32(c.count);
                w.u32(1);
                entries++;
                previous = c.count;
            }
        }
        out[entries_at] = static_cast<std::uint8_t>(entries >> 24);
        out[entries_at + 1] = static_cast<std::uint8_t>(entries >> 16);
        out[entries_at + 2] = static_cast<std::uint8_t>(entries >> 8);
        out[entries_at + 3] = static_cast<std::uint8_t>(entries);
        w.end(at);
        at = w.begin_full("stsz", 0, 0);
        w.u32(0);
        w.u32(samples);
        for (std::uint32_t i = 0; i < samples; i++) {
            w.u32(video ? m_video_samples[i].size : m_audio_samples[i].size);
        }
        w.end(at);
        at = w.begin_full("stco", 0, 0);
        std::uint32_t chunks = 0;
        for (const Chunk &c : m_chunks) {
            chunks += c.video == video ? 1 : 0;
        }
        w.u32(chunks);
        for (const Chunk &c : m_chunks) {
            if (c.video == video) {
                w.u32(static_cast<std::uint32_t>(data_offset + c.start));
            }
        }
        w.end(at);
        w.end(stbl);
        w.end(minf);
        w.end(mdia);
        w.end(trak);
    }
    w.end(moov);

    // The sample data follows the 8-byte header of the mdat box.
    w.u32(static_cast<std::uint32_t>(8 + m_data_size));
    w.text("mdat");
}

bool Mp4File::open(Source &video, Source *audio, const Options &options)
{
    m_video = &video;
    m_audio = audio;
    m_options = options;
    m_error.clear();
    m_video_samples.clear();
    m_audio_samples.clear();
    m_sps.clear();
    m_pps.clear();
    m_chunks.clear();
    m_header.clear();
    m_skipped = 0;
    m_sample_index = -1;
    m_size = 0;

    if (!scan_video() || !scan_audio()) {
        return false;
    }

    const std::uint32_t nv = video_frames();
    std::uint32_t duration_ms = options.video_duration_ms;
    if (duration_ms == 0) {
        duration_ms = audio_frames() > 0
                          ? static_cast<std::uint32_t>(
                                static_cast<std::uint64_t>(audio_frames()) *
                                kAacSamples * 1000 / m_audio_rate)
                          : nv * 40;
    }
    m_video_delta = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(duration_ms) * (kVideoTimescale / 1000) +
            nv / 2) /
               nv));
    const std::uint32_t video_ms =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(nv) *
                                   m_video_delta * 1000 / kVideoTimescale);
    const std::uint32_t audio_ms =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(audio_frames()) *
                                   kAacSamples * 1000 / m_audio_rate);
    m_duration_ms = std::max(video_ms, audio_ms);

    plan_chunks();
    // The header does not change in size with the offsets in it.
    std::vector<std::uint8_t> probe;
    build_header(probe, 0);
    build_header(m_header, probe.size());
    m_size = m_header.size() + m_data_size;
    if (m_size > 0xffffffffull) {
        return fail("the MP4 file would be too large");
    }

    std::uint32_t longest_source = 0;
    std::uint32_t longest_sample = 0;
    for (const VideoSample &s : m_video_samples) {
        longest_source = std::max(longest_source, s.source_length);
        longest_sample = std::max(longest_sample, s.size);
    }
    m_raw.resize(longest_source);
    m_sample.resize(longest_sample);
    return true;
}

bool Mp4File::load_video_sample(std::uint32_t index)
{
    if (m_sample_index == static_cast<std::int64_t>(index)) {
        return true;
    }
    m_sample_index = -1;
    const VideoSample &s = m_video_samples[index];
    if (!m_video->read(s.source_begin, m_raw.data(), s.source_length)) {
        return false;
    }
    // Copy the slices with a length in place of the start code.
    std::size_t out = 0;
    unsigned zeros = 0;
    std::size_t nal = 0;
    int type = -1;
    bool in_nal = false;
    bool ok = true;
    auto finish = [&](std::size_t end) {
        if (in_nal && (type == 1 || type == 5)) {
            const std::size_t length = end - nal;
            if (out + 4 + length > s.size) {
                ok = false;
                return;
            }
            m_sample[out++] = static_cast<std::uint8_t>(length >> 24);
            m_sample[out++] = static_cast<std::uint8_t>(length >> 16);
            m_sample[out++] = static_cast<std::uint8_t>(length >> 8);
            m_sample[out++] = static_cast<std::uint8_t>(length);
            std::memcpy(&m_sample[out], &m_raw[nal], length);
            out += length;
        }
        in_nal = false;
    };
    // The same start code rule as in scan_video(): the zeros before a 1 are
    // part of the start code (at most three), the NAL unit ends before them.
    for (std::size_t i = 0; i < s.source_length && ok; i++) {
        const std::uint8_t b = m_raw[i];
        if (b == 1 && zeros >= 2) {
            finish(i - std::min(zeros, 3u));
            if (i + 1 < s.source_length) {
                nal = i + 1;
                type = m_raw[nal] & 0x1f;
                in_nal = true;
            }
            zeros = 0;
            continue;
        }
        zeros = b == 0 ? zeros + 1 : 0;
    }
    if (ok) {
        finish(s.source_length);
    }
    if (!ok || out != s.size) {
        return false;
    }
    m_sample_index = index;
    return true;
}

std::int64_t Mp4File::read_data(std::uint64_t offset, std::uint8_t *destination,
                                std::size_t length)
{
    // The chunk that holds the offset.
    std::size_t lo = 0;
    std::size_t hi = m_chunks.size();
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (m_chunks[mid].start <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const Chunk &chunk = m_chunks[lo];
    std::uint64_t start = chunk.start;
    for (std::uint32_t i = chunk.first; i < chunk.first + chunk.count; i++) {
        const std::uint32_t size =
            chunk.video ? m_video_samples[i].size : m_audio_samples[i].size;
        if (offset < start + size) {
            const std::size_t inner = static_cast<std::size_t>(offset - start);
            const std::size_t n = std::min<std::size_t>(length, size - inner);
            if (chunk.video) {
                if (!load_video_sample(i)) {
                    return -1;
                }
                std::memcpy(destination, &m_sample[inner], n);
            } else if (!m_audio->read(m_audio_samples[i].source_begin + inner,
                                      destination, n)) {
                return -1;
            }
            return static_cast<std::int64_t>(n);
        }
        start += size;
    }
    return -1;
}

std::int64_t Mp4File::read(std::uint64_t position, std::uint8_t *destination,
                           std::size_t length)
{
    if (position >= m_size) {
        return 0;
    }
    length = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, m_size - position));
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t at = position + done;
        if (at < m_header.size()) {
            const std::size_t n = static_cast<std::size_t>(
                std::min<std::uint64_t>(length - done, m_header.size() - at));
            std::memcpy(destination + done,
                        &m_header[static_cast<std::size_t>(at)], n);
            done += n;
            continue;
        }
        const std::int64_t n =
            read_data(at - m_header.size(), destination + done, length - done);
        if (n <= 0) {
            return -1;
        }
        done += static_cast<std::size_t>(n);
    }
    return static_cast<std::int64_t>(done);
}

}  // namespace mp4
