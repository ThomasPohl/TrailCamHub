#include <doctest/doctest.h>

#include "data_usage.hpp"
#include "uplink_protocol.hpp"

using namespace uplink;
using namespace uplink::usage;

TEST_CASE("data usage: counts per transport with the header estimate")
{
    Counter counter;
    counter.add_request(Transport::modem, 1000, 200);
    counter.add_request(Transport::modem, 0, 100);
    counter.add_request(Transport::wifi, 10, 0);
    const Totals &t = counter.totals();
    CHECK(t.by_transport[1].tx == 1000 + 2 * kRequestOverheadTx);
    CHECK(t.by_transport[1].rx == 300 + 2 * kRequestOverheadRx);
    CHECK(t.by_transport[0].tx == 10 + kRequestOverheadTx);
    CHECK(t.by_transport[2].empty());
}

TEST_CASE("data usage: reported once after the acknowledgement")
{
    Counter counter;
    counter.add_request(Transport::wifi, 100, 100);
    const Totals snapshot = counter.totals();
    // Bytes counted while the check-in is under way stay pending.
    counter.add_request(Transport::wifi, 50, 50);
    counter.acknowledge(snapshot);
    CHECK(counter.totals().by_transport[0].tx == 50 + kRequestOverheadTx);
    CHECK(counter.totals().by_transport[0].rx == 50 + kRequestOverheadRx);
}

TEST_CASE("data usage: a failed check-in loses and doubles nothing")
{
    Counter counter;
    counter.add_request(Transport::modem, 500, 0);
    const Totals first = counter.totals();
    // No acknowledge(): the next snapshot contains the same bytes again.
    const Totals second = counter.totals();
    CHECK(second.by_transport[1].tx == first.by_transport[1].tx);
    counter.acknowledge(second);
    CHECK(counter.totals().empty());
    // Acknowledging more than is pending does not wrap around.
    counter.acknowledge(second);
    CHECK(counter.totals().empty());
}

TEST_CASE("data usage: survives a restart through the stored text")
{
    Counter counter;
    counter.add_request(Transport::ethernet, 7, 9);
    const std::string text = counter.serialize();
    Counter restored;
    CHECK(restored.parse(text));
    CHECK(restored.totals().by_transport[2].tx == 7 + kRequestOverheadTx);
    CHECK(restored.totals().by_transport[2].rx == 9 + kRequestOverheadRx);
    CHECK(restored.serialize() == text);
}

TEST_CASE("data usage: malformed stored text is refused")
{
    Counter counter;
    counter.add_request(Transport::wifi, 1, 1);
    const std::string before = counter.serialize();
    CHECK_FALSE(counter.parse(""));
    CHECK_FALSE(counter.parse("1,2,3"));
    CHECK_FALSE(counter.parse("1,2,3,4,5,x"));
    CHECK_FALSE(counter.parse("1,2,3,4,5,6,7"));
    CHECK(counter.serialize() == before);
}

TEST_CASE("data usage: overhead factor rounds to the nearest byte")
{
    CHECK(with_overhead(1000, 110) == 1100);
    CHECK(with_overhead(5, 110) == 6);
    CHECK(with_overhead(0, 110) == 0);
}

TEST_CASE("data usage: the check-in carries the entries")
{
    CheckinRequest request;
    request.hub_firmware = "v1";
    CHECK(build_checkin_json(request).find("dataUsage") == std::string::npos);
    request.data_usage.push_back({"modem", 1200, 3400});
    const std::string json = build_checkin_json(request);
    CHECK(json.find("\"dataUsage\":[{\"transport\":\"modem\","
                    "\"txBytes\":1200,\"rxBytes\":3400,\"estimate\":true}]") !=
          std::string::npos);
}
