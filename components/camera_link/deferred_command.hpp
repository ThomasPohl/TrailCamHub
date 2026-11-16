#pragma once

// A command that could not be sent yet because every DATA slot of the session
// was waiting for its ACK (pppp::Session::kMaxOutstanding). The camera
// acknowledges commands late while it streams media, so the slots fill up
// between two thumbnails; a command dropped at that point was never asked
// again and the session idled until its watchdog. The command is kept here
// and sent as soon as a slot is free. Pure logic (no ESP-IDF), host-tested;
// camera_protocol.cpp sends the packet.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace camera::link
{

class DeferredCommand
{
  public:
    // Pause between two attempts after a send that failed with a free slot.
    static constexpr std::int64_t kRetryIntervalUs = 200 * 1000;

    // Keeps `packet` for later. The commands of a session follow each other,
    // so a command still held is superseded; returns whether one was.
    bool hold(std::vector<std::uint8_t> packet, std::uint32_t cmd_id,
              const char *context, std::int64_t now_us)
    {
        const bool replaced = waiting_;
        packet_ = std::move(packet);
        cmd_id_ = cmd_id;
        context_ = context != nullptr ? context : "";
        held_since_us_ = now_us;
        retry_at_us_ = now_us;
        waiting_ = true;
        return replaced;
    }

    bool waiting() const
    {
        return waiting_;
    }
    // Whether to try sending now, given that the session has a free slot.
    bool due(std::int64_t now_us) const
    {
        return waiting_ && now_us >= retry_at_us_;
    }
    // Time until the next attempt after a failed one, capped at `max_us`.
    std::int64_t time_until_retry_us(std::int64_t now_us,
                                     std::int64_t max_us) const
    {
        if (!waiting_) {
            return max_us;
        }
        const std::int64_t wait = retry_at_us_ - now_us;
        if (wait <= 0) {
            return 0;
        }
        return wait < max_us ? wait : max_us;
    }
    // Sending failed although a slot was free: try again after a pause.
    void note_send_failed(std::int64_t now_us)
    {
        retry_at_us_ = now_us + kRetryIntervalUs;
    }

    const std::vector<std::uint8_t> &packet() const
    {
        return packet_;
    }
    std::uint32_t cmd_id() const
    {
        return cmd_id_;
    }
    const char *context() const
    {
        return context_.c_str();
    }
    // How long the command has been held.
    std::int64_t held_us(std::int64_t now_us) const
    {
        return waiting_ ? now_us - held_since_us_ : 0;
    }

    // The command was sent or is no longer wanted; gives the memory back.
    void clear()
    {
        std::vector<std::uint8_t>().swap(packet_);
        std::string().swap(context_);
        cmd_id_ = 0;
        waiting_ = false;
    }

  private:
    std::vector<std::uint8_t> packet_;
    std::string context_;
    std::uint32_t cmd_id_ = 0;
    std::int64_t held_since_us_ = 0;
    std::int64_t retry_at_us_ = 0;
    bool waiting_ = false;
};

}  // namespace camera::link
