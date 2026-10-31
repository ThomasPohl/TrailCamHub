#include "playback_decoder.hpp"

#include <cstring>
#include <utility>

namespace camera::artemis
{

namespace
{

constexpr std::size_t kPageSize = 0x1000;
constexpr std::size_t kEncryptedPrefix = 0x60;
// Start codes are looked for at these positions of a video frame's data.
constexpr std::size_t kLeadPositions = 5;

// First position up to kLeadPositions - 1 where `code` starts, or size.
std::size_t find_in_lead(const std::uint8_t *data, std::size_t size,
                         const std::uint8_t *code, std::size_t code_size)
{
    for (std::size_t i = 0; i < kLeadPositions && i + code_size <= size; i++) {
        if (std::memcmp(data + i, code, code_size) == 0) {
            return i;
        }
    }
    return size;
}

}  // namespace

PlaybackDecoder::PlaybackDecoder(const Aes128 &cipher, Callbacks callbacks)
    : cipher_(cipher), callbacks_(std::move(callbacks))
{
}

void PlaybackDecoder::reset()
{
    mode_ = Mode::skip;
    collected_.clear();
}

bool PlaybackDecoder::begin(std::uint32_t version, std::uint32_t,
                            std::size_t payload_size)
{
    payload_size_ = payload_size;
    header_used_ = 0;
    info_ = FrameInfo();
    position_ = 0;
    prefix_used_ = 0;
    lead_mode_ = Lead::deciding;
    lead_used_ = 0;
    collected_.clear();
    // A record without data does not parse either.
    if (version != kPlaybackRecordVersion ||
        payload_size <= kPlaybackHeaderSize) {
        mode_ = Mode::skip;
        callbacks_.other();
        return true;
    }
    mode_ = Mode::header;
    return true;
}

bool PlaybackDecoder::data(const std::uint8_t *data, std::size_t size)
{
    while (size > 0) {
        switch (mode_) {
        case Mode::skip:
            return true;
        case Mode::header: {
            const std::size_t take = kPlaybackHeaderSize - header_used_ < size
                                         ? kPlaybackHeaderSize - header_used_
                                         : size;
            std::memcpy(header_ + header_used_, data, take);
            header_used_ += take;
            data += take;
            size -= take;
            if (header_used_ < kPlaybackHeaderSize) {
                return true;
            }
            // Only the header is read; the size gives the data length.
            PlaybackFrame frame;
            if (!parse_playback_record(header_, payload_size_, frame) ||
                frame.kind == PlaybackKind::other) {
                mode_ = Mode::skip;
                callbacks_.other();
                return true;
            }
            frame.data = nullptr;
            if (!callbacks_.accept(frame)) {
                mode_ = Mode::skip;
                return true;
            }
            info_.frame = frame;
            mode_ = Mode::data;
            break;
        }
        case Mode::data: {
            const std::size_t data_size = info_.frame.size;
            if (position_ >= data_size) {
                return true;  // more than the header announced
            }
            const std::size_t page_start = position_ - position_ % kPageSize;
            const std::size_t in_page = position_ - page_start;
            // As decrypt_playback_data(): only a page with at least 0x60
            // bytes left has an encrypted prefix.
            if (data_size - page_start >= kEncryptedPrefix &&
                in_page < kEncryptedPrefix) {
                const std::size_t want = kEncryptedPrefix - in_page;
                const std::size_t take = want < size ? want : size;
                std::memcpy(prefix_ + in_page, data, take);
                position_ += take;
                data += take;
                size -= take;
                if (take == want) {
                    decrypt_playback_data(cipher_, prefix_, kEncryptedPrefix);
                    if (!output(prefix_, kEncryptedPrefix)) {
                        return false;
                    }
                }
                break;
            }
            std::size_t page_end = page_start + kPageSize;
            if (page_end > data_size) {
                page_end = data_size;
            }
            const std::size_t want = page_end - position_;
            const std::size_t take = want < size ? want : size;
            if (!output(data, take)) {
                return false;
            }
            position_ += take;
            data += take;
            size -= take;
            break;
        }
        }
    }
    return true;
}

bool PlaybackDecoder::end()
{
    if (mode_ != Mode::data) {
        mode_ = Mode::skip;
        return true;
    }
    mode_ = Mode::skip;
    bool ok = true;
    if (info_.frame.kind == PlaybackKind::video) {
        if (lead_mode_ == Lead::deciding) {
            // The whole frame is shorter than the lead.
            ok = decide_lead(true);
        } else if (lead_mode_ == Lead::collect) {
            const std::size_t skip =
                annexb_start_offset(collected_.data(), collected_.size());
            ok = callbacks_.write(PlaybackKind::video, collected_.data() + skip,
                                  collected_.size() - skip);
            collected_.clear();
        }
    }
    return ok && callbacks_.finish(info_, lead_mode_ == Lead::drop);
}

bool PlaybackDecoder::output(const std::uint8_t *data, std::size_t size)
{
    if (info_.first_size < sizeof(info_.first_bytes)) {
        std::size_t take = sizeof(info_.first_bytes) - info_.first_size;
        if (take > size) {
            take = size;
        }
        std::memcpy(info_.first_bytes + info_.first_size, data, take);
        info_.first_size += take;
    }
    if (info_.frame.kind == PlaybackKind::audio) {
        return callbacks_.write(PlaybackKind::audio, data, size);
    }
    if (lead_mode_ == Lead::deciding) {
        std::size_t take = sizeof(lead_) - lead_used_;
        if (take > size) {
            take = size;
        }
        std::memcpy(lead_ + lead_used_, data, take);
        lead_used_ += take;
        data += take;
        size -= take;
        if (lead_used_ < sizeof(lead_)) {
            return true;
        }
        if (!decide_lead(false)) {
            return false;
        }
    }
    switch (lead_mode_) {
    case Lead::stream:
        return size == 0 || callbacks_.write(PlaybackKind::video, data, size);
    case Lead::collect:
        collected_.insert(collected_.end(), data, data + size);
        return true;
    case Lead::drop:
    case Lead::deciding:
        return true;
    }
    return true;
}

bool PlaybackDecoder::decide_lead(bool complete)
{
    if (is_marker_frame(lead_, lead_used_)) {
        lead_mode_ = Lead::drop;
        return true;
    }
    std::size_t skip = 0;
    if (complete) {
        // The lead is the whole frame: exactly annexb_start_offset().
        skip = annexb_start_offset(lead_, lead_used_);
    } else {
        // annexb_start_offset() looks for the first long start code in the
        // whole frame, then for the first short one. A long one at 0-4
        // decides it; with none there, a short one at 0 (or none at 0-4)
        // means no prefix either way. A short one at 1-4 counts only if no
        // long one follows anywhere, which is known at the end.
        static const std::uint8_t kLong[] = {0, 0, 0, 1};
        static const std::uint8_t kShort[] = {0, 0, 1};
        const std::size_t long_at =
            find_in_lead(lead_, lead_used_, kLong, sizeof(kLong));
        const std::size_t short_at =
            find_in_lead(lead_, lead_used_, kShort, sizeof(kShort));
        if (long_at < kLeadPositions) {
            skip = long_at;
        } else if (short_at > 0 && short_at < kLeadPositions) {
            lead_mode_ = Lead::collect;
            collected_.assign(lead_, lead_ + lead_used_);
            return true;
        }
    }
    lead_mode_ = Lead::stream;
    return lead_used_ == skip ||
           callbacks_.write(PlaybackKind::video, lead_ + skip,
                            lead_used_ - skip);
}

}  // namespace camera::artemis
