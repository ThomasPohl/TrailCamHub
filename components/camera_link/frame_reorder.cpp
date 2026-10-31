#include "frame_reorder.hpp"

#include <algorithm>
#include <cstring>

namespace camera::artemis
{

FrameReorder::FrameReorder(std::size_t window_frames) : window_(window_frames)
{
}

void FrameReorder::reset()
{
    std::fill(sizes_.begin(), sizes_.end(), 0);
    stats_ = Stats();
    next_frame_ = 0;
    limit_ = 0;
    pending_ = 0;
    base_seq_ = 0;
    started_ = false;
}

void FrameReorder::start(std::uint16_t base_seq, std::uint64_t frame_limit)
{
    if (sizes_.size() != window_) {
        buffer_.assign(window_ * kMaxFramePayload, 0);
        sizes_.assign(window_, 0);
    }
    reset();
    base_seq_ = base_seq;
    limit_ = frame_limit;
    started_ = true;
}

FrameReorder::Outcome FrameReorder::feed(std::uint16_t seq,
                                         const std::uint8_t *payload,
                                         std::size_t size,
                                         const Deliver &deliver)
{
    if (size > kMaxFramePayload) {
        size = kMaxFramePayload;
    }
    // Distance from the frame expected next, as a signed 16-bit value so that
    // the camera's running sequence may wrap during the stream.
    const std::uint16_t expected =
        static_cast<std::uint16_t>(base_seq_ + next_frame_);
    const std::int16_t delta = static_cast<std::int16_t>(seq - expected);
    if (delta < 0) {
        stats_.duplicates++;
        return Outcome::duplicate;
    }
    const std::uint64_t index = next_frame_ + static_cast<std::uint64_t>(delta);
    if (index >= limit_) {
        stats_.outside++;
        return Outcome::outside;
    }
    if (delta > 0) {
        if (static_cast<std::size_t>(delta) > window_) {
            stats_.ahead++;
            return Outcome::ahead;
        }
        stats_.buffered++;
        const std::size_t slot = index % window_;
        if (sizes_[slot] == 0 && size > 0) {
            std::memcpy(&buffer_[slot * kMaxFramePayload], payload, size);
            sizes_[slot] = static_cast<std::uint16_t>(size);
            ++pending_;
        }
        return Outcome::buffered;
    }

    if (!deliver(next_frame_, payload, size)) {
        return Outcome::failed;
    }
    ++next_frame_;
    while (next_frame_ < limit_ && window_ > 0) {
        const std::size_t slot = next_frame_ % window_;
        if (sizes_[slot] == 0) {
            break;
        }
        const std::size_t slot_size = sizes_[slot];
        sizes_[slot] = 0;
        --pending_;
        if (!deliver(next_frame_, &buffer_[slot * kMaxFramePayload],
                     slot_size)) {
            return Outcome::failed;
        }
        ++next_frame_;
    }
    return Outcome::delivered;
}

}  // namespace camera::artemis
