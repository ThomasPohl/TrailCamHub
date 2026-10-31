#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "artemis_crypto.hpp"
#include "playback_record.hpp"
#include "playback_stream.hpp"

using namespace camera::artemis;

namespace
{

void put_u32(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::vector<std::uint8_t> record(std::uint32_t ver, std::uint32_t typ,
                                 const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> out = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    out.resize(20);
    put_u32(out, 8, ver);
    put_u32(out, 12, typ);
    put_u32(out, 16, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct Collector {
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<std::uint32_t> types;
    PlaybackStream::RecordSink sink = [this](std::uint32_t, std::uint32_t typ,
                                             std::uint8_t *data,
                                             std::size_t size) {
        types.push_back(typ);
        payloads.emplace_back(data, data + size);
        return true;
    };
};

// Splits a byte stream into frames of `frame` bytes.
std::vector<std::vector<std::uint8_t>>
frames(const std::vector<std::uint8_t> &stream, std::size_t frame)
{
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t at = 0; at < stream.size(); at += frame) {
        const std::size_t n =
            stream.size() - at < frame ? stream.size() - at : frame;
        out.emplace_back(stream.begin() + at, stream.begin() + at + n);
    }
    return out;
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

}  // namespace

TEST_CASE("playback stream passes on records that span and share frames")
{
    std::vector<std::uint8_t> stream;
    for (std::uint32_t i = 0; i < 5; i++) {
        const std::vector<std::uint8_t> r =
            record(4, i, std::vector<std::uint8_t>(300 + 700 * i, i));
        stream.insert(stream.end(), r.begin(), r.end());
    }
    const auto parts = frames(stream, 1024);
    PlaybackStream playback(8, 1 << 20);
    Collector c;
    bool ack = false;
    // Out of order within the window, sequence wrapping past 0xffff.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < parts.size(); i++)
        order.push_back(i);
    std::swap(order[1], order[2]);
    for (std::size_t i : order) {
        const auto seq = static_cast<std::uint16_t>(0xfffe + i);
        CHECK(playback.feed(seq, parts[i].data(), parts[i].size(), c.sink,
                            ack) == PlaybackStream::Status::ok);
        CHECK(ack);
    }
    REQUIRE(c.payloads.size() == 5);
    for (std::uint32_t i = 0; i < 5; i++) {
        CHECK(c.types[i] == i);
        CHECK(c.payloads[i].size() == 300 + 700 * i);
        CHECK(c.payloads[i].front() == i);
    }
    CHECK(playback.records() == 5);
}

TEST_CASE("playback stream counts gaps, repeats and frames beyond the window")
{
    const std::vector<std::uint8_t> stream =
        record(4, 1, std::vector<std::uint8_t>(9000, 7));
    const auto parts = frames(stream, 1024);  // 9 frames
    REQUIRE(parts.size() >= 9);
    PlaybackStream playback(2, 1 << 20);
    Collector c;
    bool ack = false;
    playback.feed(0, parts[0].data(), parts[0].size(), c.sink, ack);
    playback.feed(2, parts[2].data(), parts[2].size(), c.sink, ack);  // gap
    CHECK(playback.pending_frames() == 1);
    playback.feed(2, parts[2].data(), parts[2].size(), c.sink, ack);  // again
    CHECK(playback.pending_frames() == 1);
    playback.feed(0, parts[0].data(), parts[0].size(), c.sink, ack);  // repeat
    playback.feed(7, parts[7].data(), parts[7].size(), c.sink, ack);  // far
    CHECK_FALSE(ack);
    // Every frame ahead of a gap counts, the repeated one as well.
    CHECK(playback.reorder_stats().buffered == 2);
    CHECK(playback.reorder_stats().duplicates == 1);
    CHECK(playback.reorder_stats().ahead == 1);
    playback.feed(1, parts[1].data(), parts[1].size(), c.sink, ack);  // fills
    CHECK(ack);
    CHECK(playback.pending_frames() == 0);
    CHECK(playback.frames() == 3);
    playback.reset();
    CHECK(playback.reorder_stats().buffered == 0);
}

TEST_CASE("playback stream waits for a frame that starts a record")
{
    const std::vector<std::uint8_t> stream =
        record(4, 1, std::vector<std::uint8_t>(1500, 7));
    const auto parts = frames(stream, 1024);
    PlaybackStream playback(8, 1 << 20);
    Collector c;
    bool ack = true;
    // The second frame first: not a record start, not acknowledged.
    CHECK(playback.feed(11, parts[1].data(), parts[1].size(), c.sink, ack) ==
          PlaybackStream::Status::ok);
    CHECK_FALSE(ack);
    CHECK_FALSE(playback.started());
    playback.feed(10, parts[0].data(), parts[0].size(), c.sink, ack);
    CHECK(ack);
    playback.feed(11, parts[1].data(), parts[1].size(), c.sink, ack);
    CHECK(c.payloads.size() == 1);
}

TEST_CASE("playback stream gives up when the records lose sync")
{
    std::vector<std::uint8_t> stream =
        record(4, 1, std::vector<std::uint8_t>(100, 1));
    stream.insert(stream.end(), 30, 0x55);  // not a header
    PlaybackStream playback(8, 1 << 20);
    Collector c;
    bool ack = false;
    CHECK(playback.feed(0, stream.data(), stream.size(), c.sink, ack) ==
          PlaybackStream::Status::invalid);
    CHECK(c.payloads.size() == 1);
}

TEST_CASE("playback record header: video, audio and other")
{
    std::vector<std::uint8_t> video(kPlaybackHeaderSize + 50, 0);
    put_u32(video, 8, 1234);
    put_u32(video, 16, 50);
    put_u32(video, 28, 1920);
    put_u32(video, 32, 1080);
    put_u32(video, 48, 37946);
    PlaybackFrame frame;
    REQUIRE(parse_playback_record(video.data(), video.size(), frame));
    CHECK(frame.kind == PlaybackKind::video);
    CHECK(frame.pts_ms == 1234);
    CHECK(frame.width == 1920);
    CHECK(frame.session_no == 37946);
    CHECK(frame.size == 50);
    CHECK(frame.data == video.data() + kPlaybackHeaderSize);

    std::vector<std::uint8_t> audio(kPlaybackHeaderSize + 20, 0);
    put_u32(audio, 20, 20);
    REQUIRE(parse_playback_record(audio.data(), audio.size(), frame));
    CHECK(frame.kind == PlaybackKind::audio);

    std::vector<std::uint8_t> bad(kPlaybackHeaderSize + 20, 0);
    put_u32(bad, 16, 99);
    CHECK_FALSE(parse_playback_record(bad.data(), bad.size(), frame));
    CHECK_FALSE(parse_playback_record(bad.data(), 50, frame));
}

TEST_CASE("decrypt_playback_data restores each page prefix, not short tails")
{
    const Aes128 cipher = command_cipher();
    // Two full pages, then a 0x5f-byte tail that stays plain.
    std::vector<std::uint8_t> plain(0x2000 + 0x5f);
    for (std::size_t i = 0; i < plain.size(); i++)
        plain[i] = static_cast<std::uint8_t>(i * 7 + 3);
    std::vector<std::uint8_t> data = plain;
    encrypt_pages(cipher, data);
    CHECK(std::memcmp(data.data(), plain.data(), 0x60) != 0);
    CHECK(std::memcmp(data.data() + 0x60, plain.data() + 0x60, 0x1000 - 0x60) ==
          0);
    decrypt_playback_data(cipher, data.data(), data.size());
    CHECK(data == plain);

    // A tail of exactly 0x60 bytes is encrypted.
    std::vector<std::uint8_t> tail(0x1000 + 0x60, 0x42);
    std::vector<std::uint8_t> sealed = tail;
    encrypt_pages(cipher, sealed);
    CHECK(sealed[0x1000] != 0x42);
    decrypt_playback_data(cipher, sealed.data(), sealed.size());
    CHECK(sealed == tail);
}

TEST_CASE("marker frame and Annex-B start")
{
    const std::uint8_t marker[] = {0, 0, 0, 1, 1, 9};
    CHECK(is_marker_frame(marker, sizeof(marker)));
    const std::uint8_t idr[] = {0, 0, 0, 1, 0x65, 1};
    CHECK_FALSE(is_marker_frame(idr, sizeof(idr)));
    CHECK(annexb_start_offset(idr, sizeof(idr)) == 0);
    const std::uint8_t prefixed[] = {0xaa, 0xbb, 0, 0, 0, 1, 0x41};
    CHECK(annexb_start_offset(prefixed, sizeof(prefixed)) == 2);
    const std::uint8_t late[] = {1, 2, 3, 4, 5, 6, 0, 0, 1, 0x41};
    CHECK(annexb_start_offset(late, sizeof(late)) == 0);
}
