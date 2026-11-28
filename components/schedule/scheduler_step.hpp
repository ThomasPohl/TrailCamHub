#pragma once
// One look of the scheduler task at the schedule while the session
// controller is idle (#446): start a cycle, sleep, or wait. Pure, so that
// the host test can run the hub's wake-ups over virtual days; the task in
// components/scheduler acts on the result.
#include <cstdint>

#include "schedule_plan.hpp"

namespace schedule
{

struct StepLimits {
    // Deep sleep between cycles is built in (CONFIG_POWER_DEEP_SLEEP).
    bool deep_sleep = false;
    // Awake after a person used the hub (s).
    std::int64_t awake_after_activity_s = 0;
    // Shortest sleep worth it, and the longest (s).
    std::int64_t min_sleep_s = 0;
    std::int64_t max_sleep_s = 0;
    // Longest wait between two looks, and the wait after a request (ms).
    std::uint32_t max_wait_ms = 30000;
    std::uint32_t request_wait_ms = 5000;
};

struct StepInput {
    std::int64_t now = 0;
    Times times;
    // No cycle before this (time for the installer after a power-on).
    std::int64_t start_at = 0;
    // Awake at least until this after the boot (no sleep before).
    std::int64_t awake_until_boot = 0;
    // Last use of the hub by a person; kNever if none.
    std::int64_t last_activity = kNever;
    // Whether the hub sleeps on its current power (asked only when it could
    // sleep now, because it reads the settings).
    bool (*sleeps)(void *context) = nullptr;
    void *sleeps_context = nullptr;
};

struct Step {
    enum class Action {
        wait,     // look again after wait_ms
        camera,   // request a scan
        checkin,  // request an upload
        sleep,    // deep sleep for sleep_s
    };
    Action action = Action::wait;
    std::int64_t sleep_s = 0;
    // For wait; after a request the wait if it was accepted.
    std::uint32_t wait_ms = 0;
};

Step scheduler_step(const StepInput &input, const StepLimits &limits);

// The earlier of two schedule times (kNever: none).
std::int64_t earliest(std::int64_t a, std::int64_t b);

}  // namespace schedule
