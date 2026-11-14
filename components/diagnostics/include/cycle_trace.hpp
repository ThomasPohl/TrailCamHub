#pragma once

// Follows the cycles of the hub so that a cycle can be traced afterwards
// (#221): a number that goes on across deep sleep and marks the cycle's log
// lines, the lowest free heap and stack of the cycle, and one line per cycle
// in /sdcard/logs/cycles.jsonl (docs/sd-layout.md).
//
// main.cpp feeds it from the session status; the scheduler names its
// requests; the console and the check-in read the figures.

#include <cstdint>
#include <string>

namespace cycle_trace
{

// Restores the cycle number and, unless the hub woke from deep sleep, writes
// the boot line to cycles.jsonl. Call once the SD log runs.
void init();

// Who asks for the next cycle ("schedule"); nullptr takes it back when the
// request was refused. A cycle nobody named counts as "request".
void note_trigger(const char *trigger);

// The session controller left idle / ended the cycle. `kind` is "scan",
// "list", "actions" or "checkin"; `result` the controller's result name.
// Called from the controller task; both return quickly.
void on_cycle_started(const char *kind);
void on_cycle_finished(const char *result, std::uint32_t cameras_done,
                       std::uint32_t cameras_read);

// The memory figures as they are now, for the check-in.
struct Memory {
    // The running cycle, or the last one (0: none yet).
    std::uint32_t cycle = 0;
    // Lowest free heap of that cycle so far in bytes; PSRAM -1: none.
    std::int64_t heap_min_internal = 0;
    std::int64_t heap_min_psram = -1;
    // Lowest free stack in bytes since the tasks started; -1: unknown.
    std::int32_t stack_session = -1;
    std::int32_t stack_uplink = -1;
    std::int32_t stack_controller = -1;
};
Memory memory();

// For the console's status: the last finished cycle ("cycle 12 scan
// (schedule): uplink-done, 42 s, ..."), or "no cycle yet", and the running
// one if there is one.
std::string status_text();

}  // namespace cycle_trace
