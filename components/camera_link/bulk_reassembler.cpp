#include "bulk_reassembler.hpp"

#include <cstring>
#include <new>

namespace camera::artemis
{

namespace
{

constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint8_t *allocate_nothrow(std::size_t size)
{
    return new (std::nothrow) std::uint8_t[size];
}

}  // namespace

BulkReassembler::BulkReassembler(Allocate allocate)
    : allocate_(allocate != nullptr ? allocate : allocate_nothrow)
{
}

bool BulkReassembler::reserve(std::size_t size)
{
    if (capacity_ >= size) {
        return true;
    }
    buffer_.reset();
    capacity_ = 0;
    buffer_.reset(allocate_(size));
    if (!buffer_) {
        return false;
    }
    capacity_ = size;
    return true;
}

const char *BulkReassembler::reason_name(InvalidReason reason)
{
    switch (reason) {
    case InvalidReason::none:
        return "none";
    case InvalidReason::short_header_frame:
        return "short header frame";
    case InvalidReason::bad_signature:
        return "bad signature";
    case InvalidReason::bad_length:
        return "bad length";
    case InvalidReason::no_memory:
        return "no memory for the record";
    }
    return "unknown";
}

void BulkReassembler::reset()
{
    seen_.clear();
    received_frames_ = 0;
    received_bytes_ = 0;
    total_ = 0;
    total_frames_ = 0;
    ver_ = 0;
    typ_ = 0;
    len_ = 0;
    base_seq_ = 0;
    have_header_ = false;
    invalid_ = false;
    invalid_reason_ = InvalidReason::none;
}

void BulkReassembler::place_frame(std::uint16_t seq,
                                  const std::uint8_t *payload, std::size_t size)
{
    // The camera's bulk sequence is a running counter, not record-relative, so
    // address each frame by its distance from the header frame's sequence.
    const std::uint16_t rel = static_cast<std::uint16_t>(seq - base_seq_);
    if (rel >= total_frames_) {
        return;  // stale, wrapped, or belongs to another record
    }
    const std::size_t offset = static_cast<std::size_t>(rel) * kFramePayload;
    if (offset >= total_) {
        return;
    }
    // A frame cannot reach past the record, whatever its size says.
    const std::size_t room = total_ - offset;
    std::memcpy(buffer_.get() + offset, payload, size < room ? size : room);
    if (!seen_[rel]) {
        seen_[rel] = true;
        ++received_frames_;
        received_bytes_ += size;
    }
}

BulkReassembler::Status BulkReassembler::feed(std::uint16_t seq,
                                              const std::uint8_t *payload,
                                              std::size_t size)
{
    if (invalid_) {
        return Status::invalid;
    }
    if (payload == nullptr || size == 0) {
        return Status::incomplete;
    }

    if (!have_header_) {
        // The first frame of a record carries the 20-byte ARTEMIS header and
        // sets the base sequence; anything else here is not a record start.
        if (size < 20) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::short_header_frame;
            return Status::invalid;
        }
        if (std::memcmp(payload, kSignature, sizeof(kSignature)) != 0) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::bad_signature;
            return Status::invalid;
        }
        ver_ = read_u32_le(payload + 8);
        typ_ = read_u32_le(payload + 12);
        len_ = read_u32_le(payload + 16);
        total_ = 20 + static_cast<std::size_t>(len_);
        if (total_ == 0 || total_ > kMaxRecordSize) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::bad_length;
            return Status::invalid;
        }
        if (!reserve(total_)) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::no_memory;
            return Status::invalid;
        }
        // The block may be the one of the last record.
        std::memset(buffer_.get(), 0, total_);
        total_frames_ = (total_ + kFramePayload - 1) / kFramePayload;
        base_seq_ = seq;
        have_header_ = true;
        seen_.assign(total_frames_, false);
    }

    place_frame(seq, payload, size);

    if (received_frames_ >= total_frames_) {
        return Status::complete;
    }
    return Status::incomplete;
}

const std::uint8_t *BulkReassembler::payload() const
{
    if (!have_header_ || !buffer_) {
        return nullptr;
    }
    return buffer_.get() + 20;
}

}  // namespace camera::artemis
