#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bulk_reassembler.hpp"
#include "camera_udp_packets.hpp"
#include "media_transfer.hpp"

using namespace camera::artemis;

namespace
{

// The first 128 bytes of a real thumbnail bulk frame (subtype 0x04, seq 0,
// body_len 1028, payload 1024) captured from the camera: ARTEMIS header
// (ver 8, typ 1, len 0x4b11 = 19217), a 72-byte media header, then the JPEG.
const std::uint8_t kRealSeq0Prefix[128] = {
    0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x11, 0x4b, 0x00, 0x00, 0x46, 0x34, 0x3a, 0x43,
    0x31, 0x3a, 0x35, 0x36, 0x3a, 0x30, 0x36, 0x3a, 0x45, 0x44, 0x3a, 0x33,
    0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x2a, 0x00, 0xc9, 0x4a, 0x00, 0x00,
    0x7c, 0x31, 0xf3, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xd8, 0xff, 0xe0,
    0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48,
    0x00, 0x48, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02};

constexpr std::size_t kFrame = BulkReassembler::kFramePayload;

// Builds a frame payload of `size` bytes whose every byte equals `fill`.
std::vector<std::uint8_t> frame(std::size_t size, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(size, fill);
}

}  // namespace

TEST_CASE("BulkReassembler parses the ARTEMIS header from the real seq-0 frame")
{
    BulkReassembler r;
    // seq 0 is a full 1024-byte frame; only its first 128 bytes are known.
    std::vector<std::uint8_t> seq0(kFrame, 0);
    std::copy(kRealSeq0Prefix, kRealSeq0Prefix + sizeof(kRealSeq0Prefix),
              seq0.begin());

    CHECK(r.feed(0, seq0.data(), seq0.size()) ==
          BulkReassembler::Status::incomplete);
    REQUIRE(r.header_ready());
    CHECK(r.version() == 8);
    CHECK(r.type() == 1);
    CHECK(r.record_len() == 19217);
    CHECK(r.total_bytes() == 20u + 19217u);
    CHECK(r.total_frames() == 19);  // ceil(19237 / 1024)
}

TEST_CASE("BulkReassembler reassembles the full thumbnail record")
{
    BulkReassembler r;
    const std::size_t total = 20 + 19217;  // 19237

    std::vector<std::uint8_t> seq0(kFrame, 0);
    std::copy(kRealSeq0Prefix, kRealSeq0Prefix + sizeof(kRealSeq0Prefix),
              seq0.begin());
    REQUIRE(r.feed(0, seq0.data(), seq0.size()) ==
            BulkReassembler::Status::incomplete);

    // Feed seq 1..17 as full frames, then the short final frame (seq 18).
    BulkReassembler::Status status = BulkReassembler::Status::incomplete;
    for (std::uint16_t s = 1; s <= 17; ++s) {
        auto f = frame(kFrame, static_cast<std::uint8_t>(s));
        status = r.feed(s, f.data(), f.size());
        CHECK(status == BulkReassembler::Status::incomplete);
    }
    const std::size_t last = total - 18 * kFrame;  // 805 bytes
    auto flast = frame(last, 0x18);
    status = r.feed(18, flast.data(), flast.size());

    REQUIRE(status == BulkReassembler::Status::complete);
    CHECK(r.received_bytes() == total);
    CHECK(r.payload_size() == 19217);

    // The record payload is the 72-byte media header + JPEG; parse it.
    MediaRecordHeader header;
    std::size_t jpeg_offset = 0;
    std::size_t jpeg_size = 0;
    REQUIRE(parse_media_record(r.payload(), r.payload_size(), header,
                               jpeg_offset, jpeg_size));
    CHECK(header.dir_num == 100);
    CHECK(header.media_num == 42);
    CHECK(jpeg_offset == 72);
    CHECK(jpeg_size == 19217 - 72);
    // JPEG SOI sits right after the 72-byte media header.
    CHECK(r.payload()[72] == 0xff);
    CHECK(r.payload()[73] == 0xd8);
}

TEST_CASE("BulkReassembler handles a running sequence that does not start at 0")
{
    // The camera's bulk sequence is a global counter: a record's first frame
    // can arrive with any base sequence (here 10). Later frames also come out
    // of order with a duplicate.
    BulkReassembler r;
    std::vector<std::uint8_t> hdr(kFrame, 0);  // header-bearing frame
    const std::uint8_t sig[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    std::copy(sig, sig + 8, hdr.begin());
    const std::uint32_t len = 2058;  // payload after the 20-byte header
    hdr[16] = len & 0xff;
    hdr[17] = (len >> 8) & 0xff;
    const std::size_t total = 20 + len;  // 2078 -> 3 frames (1024,1024,30)

    auto mid = frame(kFrame, 0xb1);
    auto last = frame(total - 2 * kFrame, 0xb2);  // 30 bytes

    CHECK(r.feed(10, hdr.data(), hdr.size()) ==
          BulkReassembler::Status::incomplete);  // base = 10
    CHECK(r.total_frames() == 3);
    CHECK(r.feed(12, last.data(), last.size()) ==
          BulkReassembler::Status::incomplete);
    CHECK(r.feed(12, last.data(), last.size()) ==
          BulkReassembler::Status::incomplete);  // duplicate, no progress
    CHECK(r.feed(11, mid.data(), mid.size()) ==
          BulkReassembler::Status::complete);
    CHECK(r.received_bytes() == total);
}

TEST_CASE("BulkReassembler rejects a frame without the ARTEMIS signature")
{
    BulkReassembler r;
    auto bad = frame(64, 0x00);
    CHECK(r.feed(0, bad.data(), bad.size()) ==
          BulkReassembler::Status::invalid);
    CHECK(r.invalid_reason() == BulkReassembler::InvalidReason::bad_signature);
}

TEST_CASE("BulkReassembler reports why a record was rejected")
{
    // Too short to hold the 20-byte header.
    BulkReassembler a;
    auto tiny = frame(8, 0x00);
    CHECK(a.feed(0, tiny.data(), tiny.size()) ==
          BulkReassembler::Status::invalid);
    CHECK(a.invalid_reason() ==
          BulkReassembler::InvalidReason::short_header_frame);

    // Valid signature but an absurd length field.
    BulkReassembler b;
    std::vector<std::uint8_t> hdr(kFrame, 0);
    const std::uint8_t sig[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    std::copy(sig, sig + 8, hdr.begin());
    hdr[16] = 0xff;
    hdr[17] = 0xff;
    hdr[18] = 0xff;
    hdr[19] = 0xff;  // len = 0xffffffff
    CHECK(b.feed(0, hdr.data(), hdr.size()) ==
          BulkReassembler::Status::invalid);
    CHECK(b.invalid_reason() == BulkReassembler::InvalidReason::bad_length);

    // The largest record that is kept in RAM, and one byte more.
    const auto header_with_len = [&](std::uint32_t len) {
        std::vector<std::uint8_t> h(kFrame, 0);
        std::copy(sig, sig + 8, h.begin());
        h[16] = static_cast<std::uint8_t>(len);
        h[17] = static_cast<std::uint8_t>(len >> 8);
        h[18] = static_cast<std::uint8_t>(len >> 16);
        h[19] = static_cast<std::uint8_t>(len >> 24);
        return h;
    };
    const std::uint32_t max_len =
        static_cast<std::uint32_t>(BulkReassembler::kMaxRecordSize) - 20;
    BulkReassembler fits;
    const std::vector<std::uint8_t> fits_header = header_with_len(max_len);
    CHECK(fits.feed(0, fits_header.data(), fits_header.size()) ==
          BulkReassembler::Status::incomplete);
    BulkReassembler too_large;
    const std::vector<std::uint8_t> large_header = header_with_len(max_len + 1);
    CHECK(too_large.feed(0, large_header.data(), large_header.size()) ==
          BulkReassembler::Status::invalid);
    CHECK(too_large.invalid_reason() ==
          BulkReassembler::InvalidReason::bad_length);

    CHECK(std::string(BulkReassembler::reason_name(
              BulkReassembler::InvalidReason::bad_signature)) ==
          "bad signature");
}

TEST_CASE("ArtemisBulkAckRequest builds a windowed ACK")
{
    camera::ArtemisBulkAckRequest ack;
    const std::uint16_t seqs[] = {0, 1, 2};
    REQUIRE(ack.build(0x04, seqs, 3));
    const std::uint8_t expected[] = {0xf1, 0xd1, 0x00, 0x0a, 0xd1, 0x04, 0x00,
                                     0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02};
    REQUIRE(ack.size() == sizeof(expected));
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        CHECK(ack.data()[i] == expected[i]);
    }
    // Too many sequences is rejected.
    std::vector<std::uint16_t> many(camera::ArtemisBulkAckRequest::kMaxSeqs + 1,
                                    0);
    CHECK_FALSE(ack.build(0x04, many.data(), many.size()));
}

namespace
{

int g_allocations = 0;

std::uint8_t *counting_allocate(std::size_t size)
{
    ++g_allocations;
    return new std::uint8_t[size];
}

std::uint8_t *failing_allocate(std::size_t)
{
    return nullptr;
}

// The header frame of a record with `len` payload bytes.
std::vector<std::uint8_t> header_frame(std::uint32_t len)
{
    std::vector<std::uint8_t> out(len + 20 < kFrame ? len + 20 : kFrame, 0x11);
    std::copy(kRealSeq0Prefix, kRealSeq0Prefix + 20, out.begin());
    out[16] = static_cast<std::uint8_t>(len);
    out[17] = static_cast<std::uint8_t>(len >> 8);
    out[18] = static_cast<std::uint8_t>(len >> 16);
    out[19] = static_cast<std::uint8_t>(len >> 24);
    return out;
}

}  // namespace

TEST_CASE("BulkReassembler refuses a record the heap cannot hold")
{
    // On a board without PSRAM the block for a thumbnail may not be there;
    // the record is refused instead of the hub restarting.
    BulkReassembler r(failing_allocate);
    const auto first = header_frame(19217);
    CHECK(r.feed(0, first.data(), first.size()) ==
          BulkReassembler::Status::invalid);
    CHECK(r.invalid_reason() == BulkReassembler::InvalidReason::no_memory);
    CHECK(r.payload() == nullptr);
    CHECK(std::string(BulkReassembler::reason_name(r.invalid_reason())) ==
          "no memory for the record");
}

TEST_CASE("BulkReassembler takes one block per record and reuses it")
{
    g_allocations = 0;
    BulkReassembler r(counting_allocate);

    // 3 frames: one block of the record's size, not one per growth step.
    const auto first = header_frame(2500);
    REQUIRE(r.feed(10, first.data(), first.size()) ==
            BulkReassembler::Status::incomplete);
    const auto middle = frame(kFrame, 0x22);
    REQUIRE(r.feed(11, middle.data(), middle.size()) ==
            BulkReassembler::Status::incomplete);
    const auto last = frame(2520 - 2 * kFrame, 0x33);
    REQUIRE(r.feed(12, last.data(), last.size()) ==
            BulkReassembler::Status::complete);
    CHECK(g_allocations == 1);
    CHECK(r.payload_size() == 2500);
    CHECK(r.payload()[0] == 0x11);
    CHECK(r.payload()[kFrame - 20] == 0x22);
    CHECK(r.payload()[2499] == 0x33);

    // A smaller record uses the same block and does not see the old bytes.
    r.reset();
    CHECK(r.payload() == nullptr);
    const auto small = header_frame(100);
    REQUIRE(r.feed(13, small.data(), small.size()) ==
            BulkReassembler::Status::complete);
    CHECK(g_allocations == 1);
    CHECK(r.payload()[99] == 0x11);

    // A larger one takes a new block.
    r.reset();
    const auto large = header_frame(5000);
    REQUIRE(r.feed(14, large.data(), large.size()) ==
            BulkReassembler::Status::incomplete);
    CHECK(g_allocations == 2);
}

TEST_CASE("BulkReassembler keeps a frame inside the record")
{
    // The last frame claims more bytes than the record has left.
    BulkReassembler r;
    const auto first = header_frame(1100);  // 1120 bytes: 2 frames
    REQUIRE(r.feed(0, first.data(), first.size()) ==
            BulkReassembler::Status::incomplete);
    const auto last = frame(kFrame, 0x44);
    REQUIRE(r.feed(1, last.data(), last.size()) ==
            BulkReassembler::Status::complete);
    CHECK(r.payload_size() == 1100);
    CHECK(r.payload()[1099] == 0x44);
}
