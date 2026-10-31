#include "bulk_stream.hpp"

#include <cstring>

namespace camera::artemis
{

namespace
{

constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
constexpr std::size_t kArtemisHeaderSize = 20;

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

BulkStream::BulkStream(std::size_t prefix_size, std::size_t reorder_frames)
    : prefix_size_(prefix_size), reorder_(reorder_frames)
{
}

void BulkStream::reset()
{
    reorder_.reset();
    prefix_.clear();
    delivered_ = 0;
    total_frames_ = 0;
    len_ = 0;
    have_header_ = false;
    failed_ = false;
}

bool BulkStream::deliver(const std::uint8_t *frame, std::size_t size,
                         bool first, const Sink &sink)
{
    if (first) {
        frame += kArtemisHeaderSize;
        size -= kArtemisHeaderSize;
    }
    // The last frame may carry padding beyond the record.
    const std::uint64_t remaining = len_ - delivered_;
    if (size > remaining) {
        size = static_cast<std::size_t>(remaining);
    }
    if (prefix_.size() < prefix_size_) {
        std::size_t take = prefix_size_ - prefix_.size();
        if (take > size) {
            take = size;
        }
        prefix_.insert(prefix_.end(), frame, frame + take);
        frame += take;
        size -= take;
        delivered_ += take;
    }
    if (size > 0) {
        if (!sink(frame, size)) {
            return false;
        }
        delivered_ += size;
    }
    return true;
}

BulkStream::Status BulkStream::feed(std::uint16_t seq,
                                    const std::uint8_t *payload,
                                    std::size_t size, const Sink &sink,
                                    bool &ack)
{
    ack = false;
    if (failed_) {
        return Status::invalid;
    }
    if (payload == nullptr || size == 0) {
        return Status::incomplete;
    }

    if (!have_header_) {
        if (size < kArtemisHeaderSize ||
            std::memcmp(payload, kSignature, sizeof(kSignature)) != 0) {
            failed_ = true;
            return Status::invalid;
        }
        len_ = read_u32_le(payload + 16);
        if (len_ == 0 || len_ > kMaxRecordSize) {
            failed_ = true;
            return Status::invalid;
        }
        const std::uint64_t total = kArtemisHeaderSize + std::uint64_t{len_};
        total_frames_ = static_cast<std::size_t>((total + kFramePayload - 1) /
                                                 kFramePayload);
        reorder_.start(seq, total_frames_);
        have_header_ = true;
    }

    const FrameReorder::Outcome outcome = reorder_.feed(
        seq, payload, size,
        [this, &sink](std::uint64_t index, const std::uint8_t *frame,
                      std::size_t frame_size) {
            return deliver(frame, frame_size, index == 0, sink);
        });
    if (outcome == FrameReorder::Outcome::failed) {
        failed_ = true;
        return Status::invalid;
    }
    ack = FrameReorder::acknowledge(outcome);
    return reorder_.complete() ? Status::complete : Status::incomplete;
}

}  // namespace camera::artemis
