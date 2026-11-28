#include "scheduler_step.hpp"

namespace schedule
{

std::int64_t earliest(std::int64_t a, std::int64_t b)
{
    if (a == kNever) {
        return b;
    }
    if (b == kNever) {
        return a;
    }
    return a < b ? a : b;
}

Step scheduler_step(const StepInput &input, const StepLimits &limits)
{
    Step step;
    const std::int64_t now = input.now;
    const Task task =
        now >= input.start_at ? due(input.times, now) : Task::none;
    if (task == Task::camera || task == Task::checkin) {
        step.action =
            task == Task::camera ? Step::Action::camera : Step::Action::checkin;
        step.wait_ms = limits.request_wait_ms;
        return step;
    }

    std::int64_t next = earliest(input.times.camera, input.times.checkin);
    if (now < input.start_at) {
        next = earliest(next, input.start_at);
    }
    if (limits.deep_sleep) {
        std::int64_t awake_until = input.awake_until_boot;
        if (input.last_activity != kNever &&
            input.last_activity + limits.awake_after_activity_s > awake_until) {
            awake_until = input.last_activity + limits.awake_after_activity_s;
        }
        if (now >= awake_until && input.sleeps != nullptr &&
            input.sleeps(input.sleeps_context)) {
            std::int64_t sleep_s =
                next == kNever ? limits.max_sleep_s : next - now;
            if (sleep_s > limits.max_sleep_s) {
                sleep_s = limits.max_sleep_s;
            }
            if (sleep_s >= limits.min_sleep_s) {
                step.action = Step::Action::sleep;
                step.sleep_s = sleep_s;
                return step;
            }
        } else if (now < awake_until) {
            next = earliest(next, awake_until);
        }
    }
    step.wait_ms = limits.max_wait_ms;
    if (next != kNever) {
        const std::int64_t left_ms = (next - now) * 1000;
        if (left_ms < (std::int64_t)step.wait_ms) {
            step.wait_ms = left_ms < 1000 ? 1000 : (std::uint32_t)left_ms;
        }
    }
    return step;
}

}  // namespace schedule
