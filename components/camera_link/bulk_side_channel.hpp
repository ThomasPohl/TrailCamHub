#pragma once

// Bulk frames the hub did not ask for. Besides the media it requests
// (subtype 0x02 video stream, 0x03 photo, 0x04 thumbnail) the camera sends
// small records on other subtypes on its own: a TC100 sent one 25 or 26 byte
// record every few seconds on subtype 0x06 during a scan (2026-11-15). Their
// meaning is not known yet. They must not be taken for requested media: such a
// record arriving between the frames of a thumbnail made the hub drop the
// thumbnail and then wait for it for minutes, because the records kept
// counting as progress. Pure logic, host-testable.

#include <cstdint>

namespace camera::artemis
{

// The subtype of the records seen so far.
inline constexpr std::uint8_t kBulkStatusSubtype = 0x06;

// Whether a bulk frame is outside the requested media. `subtype` is not the
// video stream's (0x02 is handled before). During a download every subtype
// but the thumbnails' and the known side channel is the requested file.
constexpr bool is_bulk_side_channel(std::uint8_t subtype, bool download_active)
{
    if (subtype == kBulkStatusSubtype) {
        return true;
    }
    if (download_active) {
        return false;
    }
    return subtype != 0x04;
}

// Tells the first arrival of a side-channel frame from its repeats (the
// camera sends a frame again until it is acknowledged) and counts the frames.
class BulkSideChannel
{
  public:
    void reset()
    {
        seen_ = false;
        frames_ = 0;
    }

    // Returns whether this frame is new, i.e. not the one seen last.
    bool note(std::uint8_t subtype, std::uint16_t seq)
    {
        if (seen_ && subtype == subtype_ && seq == seq_) {
            return false;
        }
        seen_ = true;
        subtype_ = subtype;
        seq_ = seq;
        ++frames_;
        return true;
    }

    // Different frames seen since reset().
    std::uint32_t frames() const
    {
        return frames_;
    }

  private:
    bool seen_ = false;
    std::uint8_t subtype_ = 0;
    std::uint16_t seq_ = 0;
    std::uint32_t frames_ = 0;
};

}  // namespace camera::artemis
