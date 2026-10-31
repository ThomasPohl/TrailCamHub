#pragma once

// Splits the playback stream of a video (bulk subtype 0x02, started with
// cmdId 769) into its ARTEMIS records. Unlike a photo, the stream is a
// sequence of records, one per video or audio frame, that follow each other
// in the byte stream regardless of frame boundaries. FrameReorder puts the
// frames in order; the payload of each record is passed on as it arrives, in
// pieces (RecordHandler), without collecting the record, or, for a
// RecordSink, as one complete record. The stream starts at the first frame
// that begins with a record header; frames before it are not acknowledged,
// so the camera sends them again. Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "frame_reorder.hpp"

namespace camera::artemis
{

class PlaybackStream
{
  public:
    // Receives one record: version, type and payload (after the 20-byte
    // header). The payload may be modified. Returns false to abort.
    using RecordSink = std::function<bool(std::uint32_t, std::uint32_t,
                                          std::uint8_t *, std::size_t)>;

    // Receives the records piece by piece: begin() with the header of each
    // record, data() with its payload in order (pieces of any size, pointing
    // into the frames), end() when the payload is complete. A method that
    // returns false aborts the stream.
    class RecordHandler
    {
      public:
        virtual ~RecordHandler() = default;
        virtual bool begin(std::uint32_t version, std::uint32_t type,
                           std::size_t payload_size) = 0;
        virtual bool data(const std::uint8_t *data, std::size_t size) = 0;
        virtual bool end() = 0;
    };

    enum class Status {
        ok,
        invalid,  // lost track of the records or the sink failed
    };

    PlaybackStream(std::size_t reorder_frames, std::size_t max_record_size);

    void reset();

    Status feed(std::uint16_t seq, const std::uint8_t *payload,
                std::size_t size, RecordHandler &handler, bool &ack);
    // Collects each record and passes it on complete.
    Status feed(std::uint16_t seq, const std::uint8_t *payload,
                std::size_t size, const RecordSink &sink, bool &ack);

    bool started() const
    {
        return reorder_.started();
    }
    std::uint64_t frames() const
    {
        return reorder_.next_frame();
    }
    std::uint16_t expected_seq() const
    {
        return reorder_.expected_seq();
    }
    std::size_t pending_frames() const
    {
        return reorder_.pending();
    }
    std::uint64_t records() const
    {
        return records_;
    }
    const FrameReorder::Stats &reorder_stats() const
    {
        return reorder_.stats();
    }

  private:
    // Collects the payload of each record for a RecordSink.
    class Collector : public RecordHandler
    {
      public:
        const RecordSink *sink = nullptr;
        bool begin(std::uint32_t version, std::uint32_t type,
                   std::size_t payload_size) override;
        bool data(const std::uint8_t *data, std::size_t size) override;
        bool end() override;
        void reset();

      private:
        std::vector<std::uint8_t> payload_;
        std::uint32_t version_ = 0;
        std::uint32_t type_ = 0;
    };

    // Splits in-order bytes into record headers and payload pieces.
    bool consume(const std::uint8_t *data, std::size_t size,
                 RecordHandler &handler);

    FrameReorder reorder_;
    std::size_t max_record_size_;
    std::uint8_t header_[20] = {};
    std::size_t header_used_ = 0;
    // Payload bytes of the current record still to come; 0 between records.
    std::size_t payload_left_ = 0;
    std::uint64_t records_ = 0;
    bool failed_ = false;
    Collector collector_;
};

}  // namespace camera::artemis
