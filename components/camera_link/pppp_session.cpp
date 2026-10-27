#include "pppp_session.hpp"

namespace camera::pppp
{

Session::Session(Transport &transport, const Config &config)
    : transport_(transport), config_(config)
{
}

void Session::reset()
{
    for (Outstanding &slot : outstanding_) {
        slot.used = false;
    }
    next_tx_index_ = 0;
    next_rx_index_ = 0;
    transport_failed_ = false;
}

bool Session::send(const UdpRequest &request, const char *context)
{
    if (!transport_.send(request.data(), request.size(), context)) {
        transport_failed_ = true;
        return false;
    }
    return true;
}

bool Session::send_data(const std::uint8_t *payload, std::size_t payload_size,
                        std::int64_t now_us, const char *context,
                        std::uint16_t *index)
{
    Outstanding *free_slot = nullptr;
    for (Outstanding &slot : outstanding_) {
        if (!slot.used) {
            free_slot = &slot;
            break;
        }
    }
    if (free_slot == nullptr || payload_size > kMaxPayloadSize ||
        !free_slot->packet.build(next_tx_index_, payload, payload_size)) {
        return false;
    }
    if (!send(free_slot->packet, context)) {
        return false;
    }
    free_slot->used = true;
    free_slot->index = next_tx_index_;
    free_slot->last_sent_us = now_us;
    free_slot->context = context;
    if (index != nullptr) {
        *index = next_tx_index_;
    }
    next_tx_index_++;
    return true;
}

bool Session::is_outstanding(std::uint16_t index) const
{
    for (const Outstanding &slot : outstanding_) {
        if (slot.used && slot.index == index) {
            return true;
        }
    }
    return false;
}

std::size_t Session::outstanding_count() const
{
    std::size_t count = 0;
    for (const Outstanding &slot : outstanding_) {
        count += slot.used ? 1 : 0;
    }
    return count;
}

std::size_t Session::poll(std::int64_t now_us)
{
    std::size_t retransmitted = 0;
    for (Outstanding &slot : outstanding_) {
        if (!slot.used ||
            now_us - slot.last_sent_us < config_.retransmit_interval_us) {
            continue;
        }
        if (!send(slot.packet, "DATA retransmission")) {
            break;
        }
        slot.last_sent_us = now_us;
        retransmitted++;
    }
    return retransmitted;
}

std::int64_t Session::time_until_retransmit_us(std::int64_t now_us,
                                               std::int64_t max_us) const
{
    std::int64_t result = max_us;
    for (const Outstanding &slot : outstanding_) {
        if (!slot.used) {
            continue;
        }
        const std::int64_t due =
            slot.last_sent_us + config_.retransmit_interval_us - now_us;
        if (due < result) {
            result = due > 0 ? due : 0;
        }
    }
    return result;
}

Session::Event Session::handle_packet(const std::uint8_t *bytes,
                                      std::size_t size)
{
    Event event;
    event.payload = bytes;
    event.payload_size = size;
    if (bytes == nullptr || size < 2 || bytes[0] != 0xf1) {
        return event;
    }

    PacketType control;
    if (parse_control_packet(bytes, size, &control)) {
        if (control == PacketType::alive) {
            const ControlRequest alive_ack(PacketType::alive_ack);
            send(alive_ack, "ALIVE ACK");
            event.type = Event::Type::alive;
        } else if (control == PacketType::close) {
            event.type = Event::Type::close;
        }
        return event;
    }
    if (bytes[1] == static_cast<std::uint8_t>(PacketType::discovery_reply)) {
        event.type = Event::Type::discovery_reply;
        return event;
    }
    if (!received_.parse(bytes, size)) {
        return event;
    }

    if (received_.is_data()) {
        event.index = received_.index();
        ArtemisAckRequest ack;
        if (ack.build(event.index)) {
            send(ack, "DATA ACK");
        }
        // Indices at or after the next expected one (modulo 2^16) are new;
        // earlier ones are retransmissions already delivered.
        const std::uint16_t ahead =
            static_cast<std::uint16_t>(event.index - next_rx_index_);
        if (ahead < 0x8000) {
            next_rx_index_ = static_cast<std::uint16_t>(event.index + 1);
            event.type = Event::Type::data;
            event.payload = received_.data() + ArtemisRudpPacket::kHeaderSize;
            event.payload_size =
                received_.size() - ArtemisRudpPacket::kHeaderSize;
        } else {
            event.type = Event::Type::duplicate_data;
            event.payload = nullptr;
            event.payload_size = 0;
        }
        return event;
    }
    if (received_.is_ack()) {
        event.type = Event::Type::ack;
        for (Outstanding &slot : outstanding_) {
            if (slot.used && received_.acknowledges(slot.index)) {
                slot.used = false;
                event.acked_count++;
            }
        }
        if (received_.ack_count() > 0) {
            const std::uint8_t *entry =
                received_.data() + ArtemisRudpPacket::kHeaderSize;
            event.index =
                static_cast<std::uint16_t>((entry[0] << 8) | entry[1]);
        }
        return event;
    }
    return event;
}

bool Session::transport_failed() const
{
    return transport_failed_;
}

}  // namespace camera::pppp
