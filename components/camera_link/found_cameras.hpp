#pragma once

// Cameras the hub heard in its BLE scans, read or not (#296). Kept on the SD
// card (/sdcard/found_cameras.csv, docs/sd-layout.md) because a check-in may
// come in a later wake-up than the scan, and sent with every check-in as
// "discovered". Pure logic, host-tested; the file is read and written by
// media_store.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camera::found
{

struct Entry {
    // "AA:BB:CC:DD:EE:FF" as printed by camera_ble.
    std::string address;
    // Advertised name, e.g. "TC100_ED3B"; empty if none was heard.
    std::string name;
    int rssi = 0;
    // When it was heard last; Unix seconds, 0 = clock not set.
    std::int64_t seen_at = 0;
};

// At most this many cameras are kept (and sent), the most recently heard; 16
// keep the check-in small on boards without PSRAM (#244).
constexpr std::size_t kMaxEntries = 16;
// A camera not heard for this long is dropped (if the clock is set).
constexpr std::int64_t kKeepS = 14 * 24 * 60 * 60;

// `stored` plus `heard`: one entry per address with the newest time and
// signal; a name heard once is kept. Drops entries older than kKeepS before
// `now` (when `now` is a set clock) and keeps the newest kMaxEntries.
std::vector<Entry> merge(const std::vector<Entry> &stored,
                         const std::vector<Entry> &heard, std::int64_t now);

// One line per camera: "address,rssi,seen_at,name". Names never contain a
// comma or a line break (they are dropped from them).
std::string serialize(const std::vector<Entry> &entries);
// Lines that do not parse are skipped.
std::vector<Entry> parse(const std::string &text);

}  // namespace camera::found
