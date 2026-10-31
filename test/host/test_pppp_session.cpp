#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pppp_session.hpp"

using namespace camera;
using namespace camera::pppp;
using Type = Session::Event::Type;

namespace
{

class FakeTransport : public Transport
{
  public:
    bool send(const std::uint8_t *data, std::size_t size,
              const char *context) override
    {
        sent.emplace_back(data, data + size);
        contexts.emplace_back(context);
        return succeed;
    }

    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::string> contexts;
    bool succeed = true;
};

std::vector<std::uint8_t> data_packet(std::uint16_t index,
                                      const std::string &payload)
{
    ArtemisDataRequest request;
    REQUIRE(request.build(
        index, reinterpret_cast<const std::uint8_t *>(payload.data()),
        payload.size()));
    return std::vector<std::uint8_t>(request.data(),
                                     request.data() + request.size());
}

std::vector<std::uint8_t> ack_packet(std::uint16_t index)
{
    ArtemisAckRequest request;
    REQUIRE(request.build(index));
    return std::vector<std::uint8_t>(request.data(),
                                     request.data() + request.size());
}

Session::Event receive(Session &session, const std::vector<std::uint8_t> &p)
{
    return session.handle_packet(p.data(), p.size());
}

const std::uint8_t kPayload[] = {'H', 'i'};

}  // namespace

TEST_CASE("DATA packets are numbered from 0 and kept until acknowledged")
{
    FakeTransport transport;
    Session session(transport);
    std::uint16_t first = 99;
    std::uint16_t second = 99;
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "Hello", &first));
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "HB", &second));
    CHECK(first == 0);
    CHECK(second == 1);
    CHECK(session.outstanding_count() == 2);
    CHECK(transport.sent[1] == data_packet(1, "Hi"));

    const Session::Event event = receive(session, ack_packet(0));
    CHECK(event.type == Type::ack);
    CHECK(event.acked_count == 1);
    CHECK(event.index == 0);
    CHECK_FALSE(session.is_outstanding(0));
    CHECK(session.is_outstanding(1));
}

TEST_CASE("unacknowledged DATA is retransmitted with the same index")
{
    FakeTransport transport;
    Session session(transport);
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 1000, "HB"));
    CHECK(session.poll(1000 + 799999) == 0);
    CHECK(session.time_until_retransmit_us(1000 + 700000, 500000) == 100000);
    CHECK(session.poll(1000 + 800000) == 1);
    REQUIRE(transport.sent.size() == 2);
    CHECK(transport.sent[1] == transport.sent[0]);
    CHECK(session.time_until_retransmit_us(801000, 500000) == 500000);

    receive(session, ack_packet(0));
    CHECK(session.poll(10000000) == 0);
    CHECK(session.time_until_retransmit_us(0, 500000) == 500000);
}

TEST_CASE("received DATA is acknowledged and duplicates are filtered")
{
    FakeTransport transport;
    Session session(transport);

    Session::Event event = receive(session, data_packet(0, "ACK"));
    CHECK(event.type == Type::data);
    CHECK(event.index == 0);
    CHECK(std::string(reinterpret_cast<const char *>(event.payload),
                      event.payload_size) == "ACK");
    REQUIRE(transport.sent.size() == 1);
    CHECK(transport.sent[0] == ack_packet(0));

    // The camera retransmits DATA it considers unacknowledged.
    event = receive(session, data_packet(0, "ACK"));
    CHECK(event.type == Type::duplicate_data);
    CHECK(transport.sent.size() == 2);
    CHECK(transport.sent[1] == ack_packet(0));

    event = receive(session, data_packet(1, "response"));
    CHECK(event.type == Type::data);
    CHECK(event.index == 1);
}

TEST_CASE("ALIVE is answered, CLOSE and discovery replies are reported")
{
    FakeTransport transport;
    Session session(transport);
    const std::vector<std::uint8_t> alive = {0xf1, 0xe0, 0x00, 0x00};
    CHECK(receive(session, alive).type == Type::alive);
    REQUIRE(transport.sent.size() == 1);
    CHECK(transport.sent[0] ==
          std::vector<std::uint8_t>{0xf1, 0xe1, 0x00, 0x00});

    const std::vector<std::uint8_t> close = {0xf1, 0xf0, 0x00, 0x00};
    CHECK(receive(session, close).type == Type::close);

    const std::vector<std::uint8_t> discovery_reply = {
        0xf1, 0x42, 0x00, 0x14, 0x4c, 0x42, 0x43, 0x53, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x43, 0x4a, 0x4a, 0x00, 0x00, 0x00,
    };
    CHECK(receive(session, discovery_reply).type == Type::discovery_reply);
    CHECK(transport.sent.size() == 1);

    const std::vector<std::uint8_t> other = {0x00, 0x01, 0x02};
    CHECK(receive(session, other).type == Type::unhandled);
}

TEST_CASE("send limits and transport failures")
{
    FakeTransport transport;
    Session session(transport);
    for (std::size_t i = 0; i < Session::kMaxOutstanding; i++) {
        REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    }
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));

    std::vector<std::uint8_t> large(Session::kMaxPayloadSize + 1);
    session.reset();
    CHECK_FALSE(session.send_data(large.data(), large.size(), 0, "DATA"));

    transport.succeed = false;
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK(session.outstanding_count() == 0);
}

TEST_CASE("a few failed sends do not fail the transport")
{
    FakeTransport transport;
    Config config;
    config.max_consecutive_send_failures = 3;
    Session session(transport, config);

    transport.succeed = false;
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK_FALSE(session.transport_failed());

    // A success in between starts the count again.
    transport.succeed = true;
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    transport.succeed = false;
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK_FALSE(session.transport_failed());

    // Failures in a row end it.
    CHECK_FALSE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    CHECK(session.transport_failed());

    session.reset();
    CHECK_FALSE(session.transport_failed());
}

TEST_CASE("a failed retransmission is repeated after the interval")
{
    FakeTransport transport;
    Config config;
    config.retransmit_interval_us = 1000;
    Session session(transport, config);
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA"));
    const std::size_t first = transport.sent.size();

    transport.succeed = false;
    CHECK(session.poll(1000) == 0);
    CHECK(transport.sent.size() == first + 1);
    // Not due again right away: no tight retry loop.
    CHECK(session.poll(1500) == 0);
    CHECK(transport.sent.size() == first + 1);
    CHECK_FALSE(session.transport_failed());

    transport.succeed = true;
    CHECK(session.poll(2000) == 1);
    CHECK(session.outstanding_count() == 1);
}

TEST_CASE("reset restarts the indices")
{
    FakeTransport transport;
    Session session(transport);
    std::uint16_t index = 0;
    session.send_data(kPayload, sizeof(kPayload), 0, "DATA", &index);
    receive(session, data_packet(0, "x"));
    session.reset();
    CHECK(session.outstanding_count() == 0);
    REQUIRE(session.send_data(kPayload, sizeof(kPayload), 0, "DATA", &index));
    CHECK(index == 0);
    CHECK(receive(session, data_packet(0, "x")).type == Type::data);
}

TEST_CASE("a non-LBCS 0x42 fragment is delivered as DATA and acknowledged")
{
    FakeTransport transport;
    Session session(transport);

    // Same DRW framing as DATA but type 0x42 (a fragment, not LBCS discovery).
    std::vector<std::uint8_t> fragment = data_packet(0, "chunk");
    fragment[1] = 0x42;

    const Session::Event event = receive(session, fragment);
    CHECK(event.type == Type::data);
    CHECK(std::string(reinterpret_cast<const char *>(event.payload),
                      event.payload_size) == "chunk");
    CHECK(transport.sent.size() == 1);  // the ACK
}
