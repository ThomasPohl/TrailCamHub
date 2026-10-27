#pragma once

// Reliable-UDP session layer of the camera protocol (PPPP/CS2 framing):
// numbers outgoing DATA packets, retransmits them until they are
// acknowledged, acknowledges received DATA, filters duplicates and answers
// ALIVE. The application protocol (ARTEMIS) builds on top of it.
// This module has no ESP-IDF dependencies so it can be unit-tested on the
// host; the UDP socket is hidden behind Transport.

#include <array>
#include <cstddef>
#include <cstdint>

#include "camera_udp_packets.hpp"

namespace camera::pppp
{

class Transport
{
  public:
    virtual ~Transport() = default;
    // Sends one UDP packet to the camera. `context` names it in logs.
    virtual bool send(const std::uint8_t *data, std::size_t size,
                      const char *context) = 0;
};

struct Config {
    // Retransmission interval for unacknowledged DATA packets.
    std::int64_t retransmit_interval_us = 800000;
};

class Session
{
  public:
    static constexpr std::size_t kMaxOutstanding = 4;
    static constexpr std::size_t kMaxPayloadSize =
        ArtemisRudpPacket::kMaxPacketSize - ArtemisRudpPacket::kHeaderSize;

    struct Event {
        enum class Type {
            // Not a session packet; `payload` is the raw packet.
            unhandled,
            // New DATA; `index`, `payload` and `payload_size` are set.
            data,
            // A DATA packet that was already received; acknowledged again.
            duplicate_data,
            // ACK; `acked_count` outstanding packets were acknowledged.
            ack,
            // ALIVE from the camera, answered with ALIVE_ACK.
            alive,
            // CLOSE: the camera ended the session.
            close,
            // Repeated discovery reply, ignored.
            discovery_reply,
        };

        Type type = Type::unhandled;
        std::uint16_t index = 0;
        const std::uint8_t *payload = nullptr;
        std::size_t payload_size = 0;
        std::size_t acked_count = 0;
    };

    explicit Session(Transport &transport, const Config &config = Config());

    // Starts a new session: DATA indices restart at 0 in both directions.
    void reset();

    // Sends a DATA packet and keeps it for retransmission until it is
    // acknowledged. Fails if all slots are in use, the payload is too large
    // or sending fails.
    bool send_data(const std::uint8_t *payload, std::size_t payload_size,
                   std::int64_t now_us, const char *context,
                   std::uint16_t *index = nullptr);
    bool is_outstanding(std::uint16_t index) const;
    std::size_t outstanding_count() const;

    // Retransmits DATA packets whose interval has elapsed. Returns the
    // number of retransmitted packets.
    std::size_t poll(std::int64_t now_us);
    // Time until the next retransmission is due, capped at `max_us`.
    std::int64_t time_until_retransmit_us(std::int64_t now_us,
                                          std::int64_t max_us) const;

    // Handles a packet received from the camera. DATA is acknowledged and
    // ALIVE is answered through the transport.
    Event handle_packet(const std::uint8_t *bytes, std::size_t size);

    // True once a send through the transport has failed.
    bool transport_failed() const;

  private:
    struct Outstanding {
        bool used = false;
        std::uint16_t index = 0;
        std::int64_t last_sent_us = 0;
        const char *context = nullptr;
        ArtemisDataRequest packet;
    };

    bool send(const UdpRequest &request, const char *context);

    Transport &transport_;
    Config config_;
    std::array<Outstanding, kMaxOutstanding> outstanding_{};
    std::uint16_t next_tx_index_ = 0;
    std::uint16_t next_rx_index_ = 0;
    ArtemisRudpPacket received_;
    bool transport_failed_ = false;
};

}  // namespace camera::pppp
