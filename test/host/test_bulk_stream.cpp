#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "bulk_stream.hpp"

using camera::artemis::BulkStream;

namespace
{

constexpr std::size_t kPrefix = 72;

// A record of `len` payload bytes, payload byte i == i % 251.
std::vector<std::uint8_t> make_record(std::uint32_t len)
{
    std::vector<std::uint8_t> record = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0,
                                        2,   0,   0,   0,   7,   0,   0,   0};
    for (int i = 0; i < 4; i++) {
        record.push_back(static_cast<std::uint8_t>(len >> (8 * i)));
    }
    for (std::uint32_t i = 0; i < len; i++) {
        record.push_back(static_cast<std::uint8_t>(i % 251));
    }
    return record;
}

std::size_t frame_count(const std::vector<std::uint8_t> &record)
{
    return (record.size() + BulkStream::kFramePayload - 1) /
           BulkStream::kFramePayload;
}

struct Harness {
    BulkStream stream{kPrefix, 4};
    std::vector<std::uint8_t> written;
    bool fail = false;
    BulkStream::Sink sink = [this](const std::uint8_t *data, std::size_t size) {
        if (fail)
            return false;
        written.insert(written.end(), data, data + size);
        return true;
    };

    BulkStream::Status feed(const std::vector<std::uint8_t> &record,
                            std::size_t frame, std::uint16_t base, bool &ack)
    {
        const std::size_t offset = frame * BulkStream::kFramePayload;
        std::size_t size = record.size() - offset;
        if (size > BulkStream::kFramePayload)
            size = BulkStream::kFramePayload;
        return stream.feed(static_cast<std::uint16_t>(base + frame),
                           record.data() + offset, size, sink, ack);
    }
};

bool matches(const std::vector<std::uint8_t> &written, std::uint32_t len)
{
    if (written.size() != len - kPrefix)
        return false;
    for (std::size_t i = 0; i < written.size(); i++) {
        if (written[i] != (i + kPrefix) % 251)
            return false;
    }
    return true;
}

}  // namespace

TEST_CASE("in-order frames are streamed after the prefix")
{
    const std::vector<std::uint8_t> record = make_record(5000);
    Harness h;
    bool ack = false;
    for (std::size_t f = 0; f + 1 < frame_count(record); f++) {
        CHECK(h.feed(record, f, 100, ack) == BulkStream::Status::incomplete);
        CHECK(ack);
    }
    CHECK(h.feed(record, frame_count(record) - 1, 100, ack) ==
          BulkStream::Status::complete);
    CHECK(h.stream.prefix().size() == kPrefix);
    CHECK(h.stream.prefix()[1] == 1);
    CHECK(h.stream.delivered_bytes() == 5000);
    CHECK(matches(h.written, 5000));
}

TEST_CASE("reordered frames within the window are buffered")
{
    const std::vector<std::uint8_t> record = make_record(5000);  // 5 frames
    Harness h;
    bool ack = false;
    for (std::size_t f : {0, 2, 3, 1}) {
        CHECK(h.feed(record, f, 0, ack) == BulkStream::Status::incomplete);
        CHECK(ack);
    }
    CHECK(h.written.size() == 4 * 1024 - 20 - kPrefix);
    CHECK(h.feed(record, 4, 0, ack) == BulkStream::Status::complete);
    CHECK(matches(h.written, 5000));
}

TEST_CASE("frames beyond the window are not acknowledged")
{
    const std::vector<std::uint8_t> record = make_record(10000);  // 10 frames
    Harness h;
    bool ack = false;
    h.feed(record, 0, 0, ack);
    CHECK(h.feed(record, 6, 0, ack) == BulkStream::Status::incomplete);
    CHECK_FALSE(ack);
    // A frame already written is acknowledged again.
    h.feed(record, 0, 0, ack);
    CHECK(ack);
    for (std::size_t f = 1; f < frame_count(record); f++) {
        h.feed(record, f, 0, ack);
    }
    CHECK(matches(h.written, 10000));
}

TEST_CASE("the sequence may wrap during a record")
{
    const std::vector<std::uint8_t> record = make_record(8000);
    Harness h;
    bool ack = false;
    BulkStream::Status status = BulkStream::Status::incomplete;
    for (std::size_t f = 0; f < frame_count(record); f++) {
        status = h.feed(record, f, 0xfffd, ack);
    }
    CHECK(status == BulkStream::Status::complete);
    CHECK(matches(h.written, 8000));
}

TEST_CASE("bad first frame or sink failure invalidates the stream")
{
    std::vector<std::uint8_t> record = make_record(3000);
    Harness h;
    bool ack = true;
    record[0] = 'X';
    CHECK(h.feed(record, 0, 0, ack) == BulkStream::Status::invalid);
    CHECK_FALSE(ack);

    record = make_record(3000);
    h.stream.reset();
    h.fail = true;
    CHECK(h.feed(record, 0, 0, ack) == BulkStream::Status::invalid);
    CHECK_FALSE(ack);
    h.fail = false;
    CHECK(h.feed(record, 1, 0, ack) == BulkStream::Status::invalid);
}
