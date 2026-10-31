#pragma once

// Reassembles a large media record streamed on the camera's bulk data channel
// (D0/D1 subtype 0x03 photo, 0x04 thumbnail). Each DRW frame carries up to
// 1024 payload bytes tagged with a 16-bit sequence; frame N holds record bytes
// [N*1024, N*1024+size). The record itself is one ARTEMIS record: the 20-byte
// header (ver/typ/len, little-endian) followed by `len` payload bytes (a
// 72-byte media header + the JPEG). Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera::artemis
{

class BulkReassembler
{
  public:
    static constexpr std::size_t kFramePayload = 1024;
    // Guard against a corrupt length field claiming an absurd record size.
    static constexpr std::size_t kMaxRecordSize = 8u * 1024u * 1024u;

    enum class Status {
        incomplete,  // more frames needed
        complete,    // the whole record has been gathered
        invalid,     // bad signature or oversized record; caller should reset
    };

    // Why the last feed() returned invalid (for diagnostics).
    enum class InvalidReason {
        none,
        short_header_frame,  // first frame too short to hold the 20-byte header
        bad_signature,       // first frame did not start with "ARTEMIS\0"
        bad_length,  // header length field was zero or implausibly large
    };
    static const char *reason_name(InvalidReason reason);

    void reset();

    // Feeds one bulk frame's payload (the bytes after the 8-byte DRW header).
    // `seq` is the frame's 16-bit sequence. Duplicate frames are ignored.
    Status feed(std::uint16_t seq, const std::uint8_t *payload,
                std::size_t size);

    bool header_ready() const
    {
        return have_header_;
    }
    std::uint32_t version() const
    {
        return ver_;
    }
    std::uint32_t type() const
    {
        return typ_;
    }
    // Length of the record payload (bytes after the 20-byte ARTEMIS header).
    std::uint32_t record_len() const
    {
        return len_;
    }
    // Total record size including the ARTEMIS header, known once
    // header_ready().
    std::size_t total_bytes() const
    {
        return total_;
    }
    std::size_t received_bytes() const
    {
        return received_bytes_;
    }
    // Number of frames expected (0 until header_ready()).
    std::size_t total_frames() const
    {
        return total_frames_;
    }
    // Sequence of the header-bearing frame.
    std::uint16_t base_seq() const
    {
        return base_seq_;
    }
    std::size_t received_frames() const
    {
        return received_frames_;
    }
    InvalidReason invalid_reason() const
    {
        return invalid_reason_;
    }

    // Valid after feed() returns complete: the record payload (72-byte media
    // header + JPEG) and its size (== record_len()).
    const std::uint8_t *payload() const;
    std::size_t payload_size() const
    {
        return len_;
    }

  private:
    // Places one frame at its record offset, relative to the header frame's
    // sequence. Frames outside the record (stale/wrapped) are ignored.
    void place_frame(std::uint16_t seq, const std::uint8_t *payload,
                     std::size_t size);

    std::vector<std::uint8_t> buffer_;
    std::vector<bool> seen_;           // indexed by frame-relative position
    std::size_t received_frames_ = 0;  // distinct in-record frames seen
    std::size_t received_bytes_ = 0;
    std::size_t total_ = 0;
    std::size_t total_frames_ = 0;
    std::uint32_t ver_ = 0;
    std::uint32_t typ_ = 0;
    std::uint32_t len_ = 0;
    std::uint16_t base_seq_ = 0;  // sequence of the header-bearing frame
    bool have_header_ = false;
    bool invalid_ = false;
    InvalidReason invalid_reason_ = InvalidReason::none;
};

}  // namespace camera::artemis
