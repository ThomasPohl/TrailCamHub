#pragma once

// Streams one large media record from the camera's bulk data channel (a full
// photo or video) without holding it in memory. The framing is that of
// BulkReassembler: frame N of the record (counted from the header-bearing
// frame's 16-bit sequence) holds record bytes [N*1024, N*1024+size), and the
// record is a 20-byte ARTEMIS header followed by `len` payload bytes.
//
// The first `prefix_size` payload bytes (the media header) are kept and can be
// read with prefix(); the remaining bytes are passed to the sink strictly in
// order. FrameReorder puts the frames in order and decides which are
// acknowledged. Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "frame_reorder.hpp"

namespace camera::artemis
{

class BulkStream
{
  public:
    static constexpr std::size_t kFramePayload = 1024;
    // Guard against a corrupt length field (FAT32 files are limited to 4 GB).
    static constexpr std::uint32_t kMaxRecordSize = 1024u * 1024u * 1024u;

    // Receives the next bytes of the record payload after the prefix. Returns
    // false to abort (for example when the SD card is full).
    using Sink = std::function<bool(const std::uint8_t *, std::size_t)>;

    enum class Status {
        incomplete,  // more frames needed
        complete,    // every byte has been passed to the sink
        invalid,     // bad first frame or the sink failed; caller should reset
    };

    BulkStream(std::size_t prefix_size, std::size_t reorder_frames);

    void reset();

    // Feeds one bulk frame's payload (the bytes after the 8-byte DRW header).
    // The first frame must be the header-bearing one. Sets `ack` to whether
    // the frame should be acknowledged.
    Status feed(std::uint16_t seq, const std::uint8_t *payload,
                std::size_t size, const Sink &sink, bool &ack);

    bool header_ready() const
    {
        return have_header_;
    }
    // Length of the record payload (prefix + streamed bytes).
    std::uint32_t record_len() const
    {
        return len_;
    }
    // Record payload bytes already passed on (prefix included).
    std::uint64_t delivered_bytes() const
    {
        return delivered_;
    }
    std::size_t total_frames() const
    {
        return total_frames_;
    }
    // The kept prefix; complete once delivered_bytes() >= prefix size.
    const std::vector<std::uint8_t> &prefix() const
    {
        return prefix_;
    }

  private:
    // Passes one in-order frame on. Returns false if the sink failed.
    bool deliver(const std::uint8_t *frame, std::size_t size, bool first,
                 const Sink &sink);

    std::size_t prefix_size_;
    FrameReorder reorder_;
    std::vector<std::uint8_t> prefix_;
    std::uint64_t delivered_ = 0;
    std::size_t total_frames_ = 0;
    std::uint32_t len_ = 0;
    bool have_header_ = false;
    bool failed_ = false;
};

}  // namespace camera::artemis
