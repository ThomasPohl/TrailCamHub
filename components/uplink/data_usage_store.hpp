#pragma once

// The hub's running data counter (#415): one instance, kept in NVS so that it
// survives deep sleep and reset. Hooks are called from the HTTP layer.

#include "data_usage.hpp"

namespace uplink::usage
{

// Loads the totals from NVS (once; later calls do nothing).
void init();
// The transport of the running sync; set before its first request.
void set_transport(Transport transport);
// One finished request: bytes of the request and response bodies.
void record(std::uint64_t body_tx, std::uint64_t body_rx);
// The unreported totals, to be sent with the check-in.
Totals snapshot();
// The backend took the check-in: the snapshot is no longer pending.
void acknowledge(const Totals &reported);
// Writes the totals to NVS if they changed (end of a sync).
void persist();

}  // namespace uplink::usage
