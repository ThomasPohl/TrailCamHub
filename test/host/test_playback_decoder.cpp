#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "artemis_crypto.hpp"
#include "playback_decoder.hpp"
#include "playback_record.hpp"
#include "playback_stream.hpp"

using namespace camera::artemis;

namespace
{

constexpr std::uint32_t kForeignSession = 99;

void put_u32(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// AES-128-CBC with a zero IV over the first 0x60 bytes of every page, as the
// camera does.
void encrypt_pages(const Aes128 &cipher, std::vector<std::uint8_t> &data)
{
    for (std::size_t page = 0; page < data.size(); page += 0x1000) {
        if (data.size() - page < 0x60)
            continue;
        std::uint8_t previous[16] = {};
        for (std::size_t block = 0; block < 0x60; block += 16) {
            std::uint8_t in[16];
            for (int i = 0; i < 16; i++)
                in[i] = data[page + block + i] ^ previous[i];
            cipher.encrypt_block(in, &data[page + block]);
            std::memcpy(previous, &data[page + block], 16);
        }
    }
}

// An ARTEMIS record around `payload`.
std::vector<std::uint8_t> record(std::uint32_t version,
                                 const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> out = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    out.resize(20);
    put_u32(out, 8, version);
    put_u32(out, 12, 1);
    put_u32(out, 16, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// A version-4 playback payload: header, then `plain` encrypted.
std::vector<std::uint8_t> frame_payload(const Aes128 &cipher, bool video,
                                        std::uint32_t session,
                                        std::vector<std::uint8_t> plain)
{
    std::vector<std::uint8_t> payload(kPlaybackHeaderSize, 0);
    put_u32(payload, 8, 40);
    put_u32(payload, video ? 16 : 20, static_cast<std::uint32_t>(plain.size()));
    if (video) {
        put_u32(payload, 28, 1920);
        put_u32(payload, 32, 1080);
    }
    put_u32(payload, 48, session);
    encrypt_pages(cipher, plain);
    payload.insert(payload.end(), plain.begin(), plain.end());
    return payload;
}

// Video data of `size` bytes starting with `lead`, the rest a pattern
// without start codes, optionally with a long start code at `long_at`.
std::vector<std::uint8_t> video_data(std::size_t size,
                                     const std::vector<std::uint8_t> &lead,
                                     std::size_t long_at = 0)
{
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; i++)
        data[i] = static_cast<std::uint8_t>(0x10 + i % 0xe0);
    for (std::size_t i = 0; i < lead.size() && i < size; i++)
        data[i] = lead[i];
    if (long_at != 0 && long_at + 4 <= size) {
        const std::uint8_t code[] = {0, 0, 0, 1};
        std::memcpy(&data[long_at], code, 4);
    }
    return data;
}

struct Output {
    std::vector<std::uint8_t> video;
    std::vector<std::uint8_t> audio;
    unsigned video_frames = 0;
    unsigned audio_frames = 0;
    unsigned markers = 0;
    unsigned others = 0;
    std::vector<std::vector<std::uint8_t>> first_bytes;
};

bool operator==(const Output &a, const Output &b)
{
    return a.video == b.video && a.audio == b.audio &&
           a.video_frames == b.video_frames &&
           a.audio_frames == b.audio_frames && a.markers == b.markers &&
           a.others == b.others && a.first_bytes == b.first_bytes;
}

// The firmware's former handling of a complete record.
void reference_record(const Aes128 &cipher, std::uint32_t version,
                      std::uint8_t *payload, std::size_t size, Output &out)
{
    PlaybackFrame frame;
    if (version != kPlaybackRecordVersion ||
        !parse_playback_record(payload, size, frame) ||
        frame.kind == PlaybackKind::other) {
        out.others++;
        return;
    }
    if (frame.session_no == kForeignSession)
        return;
    decrypt_playback_data(cipher, frame.data, frame.size);
    if (frame.kind == PlaybackKind::audio) {
        out.audio_frames++;
        out.audio.insert(out.audio.end(), frame.data, frame.data + frame.size);
        return;
    }
    if (is_marker_frame(frame.data, frame.size)) {
        out.markers++;
        return;
    }
    out.first_bytes.emplace_back(
        frame.data, frame.data + (frame.size < 16 ? frame.size : 16));
    const std::size_t skip = annexb_start_offset(frame.data, frame.size);
    out.video.insert(out.video.end(), frame.data + skip,
                     frame.data + frame.size);
    out.video_frames++;
}

Output reference(const Aes128 &cipher, const std::vector<std::uint8_t> &stream,
                 std::size_t frame_size)
{
    Output out;
    PlaybackStream playback(8, 1 << 20);
    const PlaybackStream::RecordSink sink =
        [&](std::uint32_t version, std::uint32_t, std::uint8_t *payload,
            std::size_t size) {
            reference_record(cipher, version, payload, size, out);
            return true;
        };
    bool ack = false;
    std::uint16_t seq = 100;
    for (std::size_t at = 0; at < stream.size(); at += frame_size) {
        const std::size_t n =
            stream.size() - at < frame_size ? stream.size() - at : frame_size;
        REQUIRE(playback.feed(seq++, &stream[at], n, sink, ack) ==
                PlaybackStream::Status::ok);
    }
    return out;
}

// The decoder with callbacks that collect its output into `out`.
PlaybackDecoder make_decoder(const Aes128 &cipher, Output &out)
{
    PlaybackDecoder::Callbacks callbacks;
    callbacks.accept = [](const PlaybackFrame &frame) {
        return frame.session_no != kForeignSession;
    };
    callbacks.write = [&](PlaybackKind kind, const std::uint8_t *data,
                          std::size_t size) {
        std::vector<std::uint8_t> &to =
            kind == PlaybackKind::audio ? out.audio : out.video;
        to.insert(to.end(), data, data + size);
        return true;
    };
    callbacks.finish = [&](const PlaybackDecoder::FrameInfo &info,
                           bool marker) {
        if (info.frame.kind == PlaybackKind::audio) {
            out.audio_frames++;
        } else if (marker) {
            out.markers++;
        } else {
            out.first_bytes.emplace_back(info.first_bytes,
                                         info.first_bytes + info.first_size);
            out.video_frames++;
        }
        return true;
    };
    callbacks.other = [&]() { out.others++; };
    return PlaybackDecoder(cipher, callbacks);
}

// Through PlaybackStream, in frames of `frame_size` bytes (20 to 1472, as
// the camera sends them).
Output decoded(const Aes128 &cipher, const std::vector<std::uint8_t> &stream,
               std::size_t frame_size)
{
    Output out;
    PlaybackDecoder decoder = make_decoder(cipher, out);
    PlaybackStream playback(8, 1 << 20);
    bool ack = false;
    std::uint16_t seq = 100;
    for (std::size_t at = 0; at < stream.size(); at += frame_size) {
        const std::size_t n =
            stream.size() - at < frame_size ? stream.size() - at : frame_size;
        REQUIRE(playback.feed(seq++, &stream[at], n, decoder, ack) ==
                PlaybackStream::Status::ok);
    }
    return out;
}

// Straight into the decoder, each record's payload in pieces of `piece`
// bytes.
Output decoded_directly(const Aes128 &cipher,
                        const std::vector<std::uint8_t> &stream,
                        std::size_t piece)
{
    Output out;
    PlaybackDecoder decoder = make_decoder(cipher, out);
    std::size_t at = 0;
    while (at + 20 <= stream.size()) {
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        std::memcpy(&version, &stream[at + 8], 4);
        std::memcpy(&length, &stream[at + 16], 4);
        at += 20;
        REQUIRE(decoder.begin(version, 1, length));
        for (std::size_t done = 0; done < length; done += piece) {
            const std::size_t n = length - done < piece ? length - done : piece;
            REQUIRE(decoder.data(&stream[at + done], n));
        }
        REQUIRE(decoder.end());
        at += length;
    }
    return out;
}

void append(std::vector<std::uint8_t> &stream,
            const std::vector<std::uint8_t> &record)
{
    stream.insert(stream.end(), record.begin(), record.end());
}

}  // namespace

TEST_CASE("playback decoder writes what the complete records gave")
{
    const Aes128 cipher = command_cipher();
    std::vector<std::uint8_t> stream;
    const std::vector<std::uint8_t> long_code = {0, 0, 0, 1, 0x65};
    // Sizes around the lead, the encrypted prefix and the page.
    const std::size_t sizes[] = {3,      7,      8,      9,      0x5f,
                                 0x60,   0x61,   0xfff,  0x1000, 0x1001,
                                 0x105f, 0x1060, 0x1061, 0x2345, 0x3000};
    for (const std::size_t size : sizes) {
        // A long start code at 0, or after a prefix of 1 to 4 bytes.
        for (std::size_t prefix = 0; prefix <= 4; prefix++) {
            std::vector<std::uint8_t> lead(prefix, 0xaa);
            lead.insert(lead.end(), long_code.begin(), long_code.end());
            append(stream, record(4, frame_payload(cipher, true, 7,
                                                   video_data(size, lead))));
        }
        // A short start code at 1 to 4, with or without a long one later.
        for (std::size_t prefix = 1; prefix <= 4; prefix++) {
            std::vector<std::uint8_t> lead(prefix, 0xbb);
            lead.insert(lead.end(), {0, 0, 1, 0x41});
            append(stream, record(4, frame_payload(cipher, true, 7,
                                                   video_data(size, lead))));
            append(stream,
                   record(4, frame_payload(cipher, true, 7,
                                           video_data(size, lead, 40))));
        }
        // Short code at 0, no start code, a marker frame, audio.
        append(stream,
               record(4, frame_payload(cipher, true, 7,
                                       video_data(size, {0, 0, 1, 0x41}))));
        append(stream,
               record(4, frame_payload(cipher, true, 7, video_data(size, {}))));
        append(stream,
               record(4, frame_payload(cipher, true, 7,
                                       video_data(size, {0, 0, 0, 1, 1}))));
        append(stream,
               record(4, frame_payload(cipher, false, 7,
                                       video_data(size, {0xff, 0xf1}))));
        // Another session, another version, and a header that does not match.
        append(stream, record(4, frame_payload(cipher, true, kForeignSession,
                                               video_data(size, long_code))));
        append(stream, record(3, frame_payload(cipher, true, 7,
                                               video_data(size, long_code))));
        std::vector<std::uint8_t> bad =
            frame_payload(cipher, true, 7, video_data(size, long_code));
        put_u32(bad, 16, 1);
        append(stream, record(4, bad));
    }
    append(stream, record(4, std::vector<std::uint8_t>(50, 1)));

    const Output expected = reference(cipher, stream, 1024);
    CHECK(expected.video_frames > 100);
    CHECK(expected.markers > 0);
    CHECK(expected.audio_frames == 15);
    for (const std::size_t frame_size :
         {std::size_t{20}, std::size_t{95}, std::size_t{96}, std::size_t{97},
          std::size_t{1024}, std::size_t{1472}}) {
        CAPTURE(frame_size);
        const Output got = decoded(cipher, stream, frame_size);
        CHECK(got.video.size() == expected.video.size());
        CHECK(got.audio.size() == expected.audio.size());
        CHECK(got == expected);
    }
    for (const std::size_t piece :
         {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{8},
          std::size_t{95}, std::size_t{96}, std::size_t{97}, std::size_t{4096},
          std::size_t{1 << 20}}) {
        CAPTURE(piece);
        CHECK(decoded_directly(cipher, stream, piece) == expected);
    }
}

TEST_CASE("playback decoder keeps the prefix before a short start code "
          "when a long one follows")
{
    const Aes128 cipher = command_cipher();
    const std::vector<std::uint8_t> lead = {0xcc, 0xdd, 0, 0, 1, 0x41};
    std::vector<std::uint8_t> stream =
        record(4, frame_payload(cipher, true, 7, video_data(200, lead, 100)));
    append(stream,
           record(4, frame_payload(cipher, true, 7, video_data(200, lead))));
    const Output got = decoded(cipher, stream, 64);
    REQUIRE(got.video_frames == 2);
    // The first frame keeps all 200 bytes, the second drops the 2-byte prefix.
    CHECK(got.video.size() == 200 + 198);
    CHECK(got.video[0] == 0xcc);
    CHECK(got.video[200] == 0);
}
