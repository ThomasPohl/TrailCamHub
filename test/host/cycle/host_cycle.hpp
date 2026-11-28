#pragma once
// The hub's world for the whole-cycle host test (#446): a virtual wall
// clock, a boot-relative esp_timer, NVS in memory and the reset reason. The
// driver "boots" the hub with power_on(), restart() or deep_sleep(); RTC
// memory (globals) and NVS survive them like on the chip.
#include <cstdint>
#include <string>
#include <vector>

namespace host_cycle
{

// A fresh hub: NVS erased, wall clock at `unix_s`, reset reason power on.
void power_on(std::int64_t unix_s);
// A restart (software reset): NVS and RTC memory kept, esp_timer from 0.
void restart();
// Deep sleep for `sleep_s`: the wall clock moves on, esp_timer starts from 0
// and the reset reason is deep sleep.
void deep_sleep(std::int64_t sleep_s);
// Time passing while awake.
void advance_ms(std::int64_t ms);

std::int64_t wall_unix();
// Microseconds since the last boot (what esp_timer_get_time() returns).
std::int64_t uptime_us();

// Keys stored in NVS, as "namespace/key", sorted.
std::vector<std::string> nvs_keys();
void erase_nvs();

// Log lines since the last clear_log(), "TAG: text".
std::vector<std::string> log_lines();
void clear_log();

}  // namespace host_cycle
