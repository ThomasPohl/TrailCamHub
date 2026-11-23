#pragma once

// Data volume of the hub's uplink (#415): bytes sent and received per
// transport, counted at the HTTP layer. Pure logic without ESP-IDF; the NVS
// storage and the hooks are in data_usage_store.cpp.
//
// The totals are the part not reported yet. A check-in takes a snapshot; only
// once the backend answered, that snapshot is subtracted (acknowledge()). A
// failed check-in therefore loses nothing and counts nothing twice.

#include <array>
#include <cstdint>
#include <string>

namespace uplink::usage
{

enum class Transport { wifi = 0, modem = 1, ethernet = 2 };
constexpr std::size_t kTransports = 3;

const char *transport_name(Transport transport);

struct Bytes {
    std::uint64_t tx = 0;  // sent
    std::uint64_t rx = 0;  // received
    bool empty() const
    {
        return tx == 0 && rx == 0;
    }
};

struct Totals {
    std::array<Bytes, kTransports> by_transport;
    bool empty() const;
};

// Request line, headers and the like that the HTTP layer does not see; a
// fixed estimate per request.
constexpr std::uint64_t kRequestOverheadTx = 400;
constexpr std::uint64_t kRequestOverheadRx = 250;

class Counter
{
  public:
    // Adds the body bytes of one request; the header estimate is added here.
    void add_request(Transport transport, std::uint64_t body_tx,
                     std::uint64_t body_rx);
    const Totals &totals() const
    {
        return totals_;
    }
    // Subtracts what a snapshot reported (never below 0).
    void acknowledge(const Totals &reported);
    // True if the totals changed since the last mark_saved().
    bool dirty() const
    {
        return dirty_;
    }
    void mark_saved()
    {
        dirty_ = false;
    }

    // "tx0,rx0,tx1,rx1,tx2,rx2" for NVS; parse() returns false (and leaves
    // the counter unchanged) for anything else.
    std::string serialize() const;
    bool parse(const std::string &text);

  private:
    Totals totals_;
    bool dirty_ = false;
};

// Bytes on the wire: the HTTP bytes times the estimate for TLS and TCP/IP,
// `percent` = 110 adds 10 %.
std::uint64_t with_overhead(std::uint64_t bytes, unsigned percent);

}  // namespace uplink::usage
