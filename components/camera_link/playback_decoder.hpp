#pragma once

// Decodes the records of a video playback stream while they arrive, piece by
// piece (see playback_stream.hpp and playback_record.hpp), so that no record
// has to be collected first: it reads the 108-byte header, decrypts the
// encrypted prefix of every page through a 96-byte buffer, passes the rest of
// the data on as it comes, drops marker frames and the alignment prefix of a
// video frame. The output is byte for byte that of parse_playback_record(),
// decrypt_playback_data(), is_marker_frame() and annexb_start_offset() on the
// complete record. Only when that depends on bytes not seen yet (a short start
// code within the first bytes and no long one) is the frame collected. Pure
// logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "artemis_aes128.hpp"
#include "playback_record.hpp"
#include "playback_stream.hpp"

namespace camera::artemis
{

class PlaybackDecoder : public PlaybackStream::RecordHandler
{
  public:
    // A frame's header; data and size describe the whole data, data is null.
    // first_bytes holds the first decrypted data bytes (before the alignment
    // prefix is dropped), first_size how many.
    struct FrameInfo {
        PlaybackFrame frame;
        std::uint8_t first_bytes[16] = {};
        std::size_t first_size = 0;
    };

    struct Callbacks {
        // A playback record's header is complete; returns true to write its
        // data (false skips the record, for example another session's).
        std::function<bool(const PlaybackFrame &)> accept;
        // Data of the accepted frame, in order. Returns false to abort.
        std::function<bool(PlaybackKind, const std::uint8_t *, std::size_t)>
            write;
        // The accepted frame is complete; `marker` if it was a marker frame
        // and nothing was written. Returns false to abort.
        std::function<bool(const FrameInfo &, bool marker)> finish;
        // A record that is not a usable playback record (another version or
        // kind, or a header that does not match its size).
        std::function<void()> other;
    };

    PlaybackDecoder(const Aes128 &cipher, Callbacks callbacks);

    void reset();

    bool begin(std::uint32_t version, std::uint32_t type,
               std::size_t payload_size) override;
    bool data(const std::uint8_t *data, std::size_t size) override;
    bool end() override;

  private:
    enum class Mode : std::uint8_t {
        skip,    // ignore the rest of the record
        header,  // collecting the 108-byte header
        data,    // passing the data on
    };
    // How the start of a video frame is handled.
    enum class Lead : std::uint8_t {
        deciding,  // collecting its first bytes
        stream,    // decided: written as it comes
        drop,      // marker frame
        collect,   // depends on the end: collected
    };

    // Decrypted or plain data bytes, in order.
    bool output(const std::uint8_t *data, std::size_t size);
    // Decides how the start of a video frame is handled, from lead_.
    bool decide_lead(bool complete);

    const Aes128 &cipher_;
    Callbacks callbacks_;

    Mode mode_ = Mode::skip;
    std::size_t payload_size_ = 0;
    std::uint8_t header_[kPlaybackHeaderSize] = {};
    std::size_t header_used_ = 0;

    FrameInfo info_;
    // Position in the frame's data.
    std::size_t position_ = 0;
    // Encrypted prefix of the current page being collected.
    std::uint8_t prefix_[0x60] = {};
    std::size_t prefix_used_ = 0;

    Lead lead_mode_ = Lead::deciding;
    std::uint8_t lead_[8] = {};
    std::size_t lead_used_ = 0;
    std::vector<std::uint8_t> collected_;
};

}  // namespace camera::artemis
