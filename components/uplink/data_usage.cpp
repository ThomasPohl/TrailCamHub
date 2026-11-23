#include "data_usage.hpp"

#include <cstdio>
#include <cstdlib>

namespace uplink::usage
{

const char *transport_name(Transport transport)
{
    switch (transport) {
    case Transport::wifi:
        return "wifi";
    case Transport::modem:
        return "modem";
    case Transport::ethernet:
        return "ethernet";
    }
    return "wifi";
}

bool Totals::empty() const
{
    for (const Bytes &b : by_transport) {
        if (!b.empty()) {
            return false;
        }
    }
    return true;
}

void Counter::add_request(Transport transport, std::uint64_t body_tx,
                          std::uint64_t body_rx)
{
    Bytes &b = totals_.by_transport[(std::size_t)transport];
    b.tx += body_tx + kRequestOverheadTx;
    b.rx += body_rx + kRequestOverheadRx;
    dirty_ = true;
}

void Counter::acknowledge(const Totals &reported)
{
    for (std::size_t i = 0; i < kTransports; i++) {
        Bytes &b = totals_.by_transport[i];
        const Bytes &r = reported.by_transport[i];
        b.tx = b.tx > r.tx ? b.tx - r.tx : 0;
        b.rx = b.rx > r.rx ? b.rx - r.rx : 0;
    }
    dirty_ = true;
}

std::string Counter::serialize() const
{
    std::string out;
    for (std::size_t i = 0; i < kTransports; i++) {
        if (i > 0) {
            out += ",";
        }
        out += std::to_string(totals_.by_transport[i].tx) + "," +
               std::to_string(totals_.by_transport[i].rx);
    }
    return out;
}

bool Counter::parse(const std::string &text)
{
    Totals parsed;
    const char *p = text.c_str();
    for (std::size_t i = 0; i < kTransports * 2; i++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        char *end = nullptr;
        const unsigned long long value = std::strtoull(p, &end, 10);
        Bytes &b = parsed.by_transport[i / 2];
        (i % 2 == 0 ? b.tx : b.rx) = value;
        p = end;
        if (i + 1 < kTransports * 2) {
            if (*p != ',') {
                return false;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    totals_ = parsed;
    return true;
}

std::uint64_t with_overhead(std::uint64_t bytes, unsigned percent)
{
    return (bytes * percent + 50) / 100;
}

}  // namespace uplink::usage
