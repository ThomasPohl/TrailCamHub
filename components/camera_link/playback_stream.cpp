#include "playback_stream.hpp"

#include <cstring>

namespace camera::artemis
{

namespace
{

constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
constexpr std::size_t kHeaderSize = 20;

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

PlaybackStream::PlaybackStream(std::size_t reorder_frames,
                               std::size_t max_record_size)
    : reorder_(reorder_frames), max_record_size_(max_record_size)
{
}

void PlaybackStream::reset()
{
    reorder_.reset();
    header_used_ = 0;
    payload_left_ = 0;
    records_ = 0;
    failed_ = false;
    collector_.reset();
}

bool PlaybackStream::consume(const std::uint8_t *data, std::size_t size,
                             RecordHandler &handler)
{
    while (size > 0) {
        if (payload_left_ == 0) {
            const std::size_t take = kHeaderSize - header_used_ < size
                                         ? kHeaderSize - header_used_
                                         : size;
            std::memcpy(header_ + header_used_, data, take);
            header_used_ += take;
            data += take;
            size -= take;
            if (header_used_ < kHeaderSize) {
                return true;
            }
            if (std::memcmp(header_, kSignature, sizeof(kSignature)) != 0) {
                return false;
            }
            const std::uint32_t length = read_u32_le(header_ + 16);
            if (length == 0 || length > max_record_size_) {
                return false;
            }
            header_used_ = 0;
            payload_left_ = length;
            if (!handler.begin(read_u32_le(header_ + 8),
                               read_u32_le(header_ + 12), length)) {
                return false;
            }
            continue;
        }
        const std::size_t take = payload_left_ < size ? payload_left_ : size;
        if (!handler.data(data, take)) {
            return false;
        }
        data += take;
        size -= take;
        payload_left_ -= take;
        if (payload_left_ == 0) {
            if (!handler.end()) {
                return false;
            }
            ++records_;
        }
    }
    return true;
}

bool PlaybackStream::Collector::begin(std::uint32_t version, std::uint32_t type,
                                      std::size_t payload_size)
{
    version_ = version;
    type_ = type;
    payload_.clear();
    payload_.reserve(payload_size);
    return true;
}

bool PlaybackStream::Collector::data(const std::uint8_t *data, std::size_t size)
{
    payload_.insert(payload_.end(), data, data + size);
    return true;
}

bool PlaybackStream::Collector::end()
{
    return (*sink)(version_, type_, payload_.data(), payload_.size());
}

void PlaybackStream::Collector::reset()
{
    payload_.clear();
}

PlaybackStream::Status PlaybackStream::feed(std::uint16_t seq,
                                            const std::uint8_t *payload,
                                            std::size_t size,
                                            const RecordSink &sink, bool &ack)
{
    collector_.sink = &sink;
    return feed(seq, payload, size, collector_, ack);
}

PlaybackStream::Status PlaybackStream::feed(std::uint16_t seq,
                                            const std::uint8_t *payload,
                                            std::size_t size,
                                            RecordHandler &handler, bool &ack)
{
    ack = false;
    if (failed_) {
        return Status::invalid;
    }
    if (payload == nullptr || size == 0) {
        return Status::ok;
    }
    if (!reorder_.started()) {
        if (size < kHeaderSize ||
            std::memcmp(payload, kSignature, sizeof(kSignature)) != 0) {
            return Status::ok;  // not the first frame; it comes again
        }
        reorder_.start(seq);
    }
    const FrameReorder::Outcome outcome =
        reorder_.feed(seq, payload, size,
                      [this, &handler](std::uint64_t, const std::uint8_t *frame,
                                       std::size_t frame_size) {
                          return consume(frame, frame_size, handler);
                      });
    if (outcome == FrameReorder::Outcome::failed) {
        failed_ = true;
        return Status::invalid;
    }
    ack = FrameReorder::acknowledge(outcome);
    return Status::ok;
}

}  // namespace camera::artemis
