#pragma once

// What the hub records about one cycle (a camera read, a listing, a check-in
// ...): the line of /sdcard/logs/cycles.jsonl and the memory figures. No
// ESP-IDF dependencies, so it can be unit-tested on the host.

#include <cstdint>
#include <string>

namespace cycle_trace
{

// Lowest free stack (bytes) of the tasks that run a cycle since they were
// started; -1: unknown.
struct StackMinimum {
    std::int32_t session = -1;
    std::int32_t uplink = -1;
    std::int32_t controller = -1;
};

struct Record {
    // Counts up with every cycle, across deep sleep and restarts; starts
    // again at 1 after a power loss.
    std::uint32_t number = 0;
    // Unix seconds when the cycle started; 0: the clock was not set.
    std::int64_t start = 0;
    // "scan", "list", "actions" or "checkin".
    std::string kind;
    // "schedule", or "request" (console, button, web UI, backend, Telegram).
    std::string trigger;
    // The controller's result name, e.g. "uplink-done".
    std::string result;
    std::uint32_t duration_s = 0;
    // Cameras done in the cycle and, of those, the ones read.
    std::uint32_t cameras_done = 0;
    std::uint32_t cameras_read = 0;
    // Lowest free heap during the cycle in bytes; PSRAM -1: the board has
    // none.
    std::int64_t heap_min_internal = 0;
    std::int64_t heap_min_psram = -1;
    StackMinimum stack;
};

// The lowest free heap during a cycle. ESP-IDF only keeps the minimum since
// boot: if that fell during the cycle, it is the cycle's minimum; otherwise
// the cycle stayed above it and the lowest of the samples taken during the
// cycle is the best figure there is.
std::int64_t cycle_heap_minimum(std::int64_t boot_minimum_at_start,
                                std::int64_t boot_minimum_now,
                                std::int64_t lowest_sample);

// One line of cycles.jsonl for a finished cycle, with '\n'. Unknown values
// (start, PSRAM, stacks) are left out.
std::string cycle_line(const Record &record);

// The line written when the hub starts other than by waking from deep sleep:
// why it restarted, and the last cycle before that (0: none). `time` is Unix
// seconds, 0 (left out) if the clock is not set.
std::string boot_line(const std::string &reset_reason, std::uint32_t last_cycle,
                      std::int64_t time);

// For the console's status: "cycle 12 scan (schedule): uplink-done, 42 s, 1
// of 1 cameras read; heap min 81234 B, PSRAM 3912345 B; stack free min
// session 2100 B, uplink 5400 B, controller 3300 B".
std::string describe(const Record &record);

}  // namespace cycle_trace
