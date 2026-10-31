#pragma once

// Puts the frames of a camera bulk stream back in order. Each frame carries a
// 16-bit sequence; the stream starts at the frame given to start(), and
// frames that arrive ahead of the next expected one are buffered within a
// reorder window. feed() tells the caller whether to acknowledge a frame: a
// frame beyond the window is not acknowledged, so the camera sends it again
// later. Sequence wrap-around is handled, so streams may exceed 65536 frames.
// Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace camera::artemis
{

class FrameReorder
{
  public:
    // Larger frames are cut; camera frames carry at most 1024 bytes.
    static constexpr std::size_t kMaxFramePayload = 1472;
    static constexpr std::uint64_t kUnlimited = ~std::uint64_t{0};

    // Receives the frames in order with their index from the start frame.
    // Returns false to abort.
    using Deliver =
        std::function<bool(std::uint64_t, const std::uint8_t *, std::size_t)>;

    enum class Outcome {
        delivered,  // passed on, with any buffered frames that followed it
        buffered,   // ahead of the next frame, kept in the window
        duplicate,  // already passed on
        outside,    // beyond the frame limit
        ahead,      // beyond the window; not kept
        failed,     // the deliver function returned false
    };

    // What feed() saw since start(); all but `buffered` mean lost or repeated
    // packets, which slow a transfer down.
    struct Stats {
        std::uint32_t buffered = 0;    // ahead of the next frame (a gap)
        std::uint32_t duplicates = 0;  // already passed on: sent again
        std::uint32_t ahead = 0;       // beyond the window, not kept
        std::uint32_t outside = 0;     // beyond the frame limit
    };

    explicit FrameReorder(std::size_t window_frames);

    void reset();
    // Starts the stream at `base_seq` with at most `frame_limit` frames. The
    // window buffer is allocated on the first start (one block, so that it can
    // be placed in external RAM).
    void start(std::uint16_t base_seq, std::uint64_t frame_limit = kUnlimited);

    Outcome feed(std::uint16_t seq, const std::uint8_t *payload,
                 std::size_t size, const Deliver &deliver);

    static bool acknowledge(Outcome outcome)
    {
        return outcome != Outcome::ahead && outcome != Outcome::failed;
    }

    bool started() const
    {
        return started_;
    }
    const Stats &stats() const
    {
        return stats_;
    }
    // Sequence of the frame expected next.
    std::uint16_t expected_seq() const
    {
        return static_cast<std::uint16_t>(base_seq_ + next_frame_);
    }
    // Frames kept in the window, waiting for a missing one before them.
    std::size_t pending() const
    {
        return pending_;
    }
    // Index of the frame expected next (= number of frames passed on).
    std::uint64_t next_frame() const
    {
        return next_frame_;
    }
    bool complete() const
    {
        return started_ && next_frame_ >= limit_;
    }

  private:
    std::size_t window_;
    std::vector<std::uint8_t> buffer_;  // window_ slots of kMaxFramePayload
    std::vector<std::uint16_t> sizes_;  // 0: slot empty
    std::uint64_t next_frame_ = 0;
    std::uint64_t limit_ = 0;
    std::size_t pending_ = 0;
    Stats stats_;
    std::uint16_t base_seq_ = 0;
    bool started_ = false;
};

}  // namespace camera::artemis
