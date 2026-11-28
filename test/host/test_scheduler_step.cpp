#include <doctest/doctest.h>

#include "scheduler_step.hpp"

using schedule::kNever;
using schedule::Step;
using schedule::StepInput;
using schedule::StepLimits;

namespace
{

bool yes(void *)
{
    return true;
}

bool no(void *)
{
    return false;
}

StepLimits battery_limits()
{
    StepLimits limits;
    limits.deep_sleep = true;
    limits.awake_after_activity_s = 300;
    limits.min_sleep_s = 60;
    limits.max_sleep_s = 720 * 60;
    return limits;
}

StepInput at(std::int64_t now, std::int64_t camera, std::int64_t checkin)
{
    StepInput input;
    input.now = now;
    input.times.camera = camera;
    input.times.checkin = checkin;
    input.sleeps = yes;
    return input;
}

}  // namespace

TEST_CASE("scheduler step: a due read or check-in is requested")
{
    const StepLimits limits = battery_limits();
    Step step = schedule::scheduler_step(at(1000, 1000, 5000), limits);
    CHECK(step.action == Step::Action::camera);
    CHECK(step.wait_ms == 5000);
    step = schedule::scheduler_step(at(1000, 5000, 900), limits);
    CHECK(step.action == Step::Action::checkin);

    // Not before the start delay after a power-on.
    StepInput input = at(1000, 1000, kNever);
    input.start_at = 1060;
    input.awake_until_boot = 1600;
    step = schedule::scheduler_step(input, limits);
    CHECK(step.action == Step::Action::wait);
    // The read is overdue: look again in a second, until the start.
    CHECK(step.wait_ms == 1000);
}

TEST_CASE("scheduler step: sleeps until the next cycle, within the limits")
{
    const StepLimits limits = battery_limits();
    Step step = schedule::scheduler_step(at(1000, 4600, 2800), limits);
    CHECK(step.action == Step::Action::sleep);
    CHECK(step.sleep_s == 1800);

    step = schedule::scheduler_step(at(1000, kNever, kNever), limits);
    CHECK(step.action == Step::Action::sleep);
    CHECK(step.sleep_s == 720 * 60);

    // Too short to sleep: wait instead.
    step = schedule::scheduler_step(at(1000, 1030, kNever), limits);
    CHECK(step.action == Step::Action::wait);
    CHECK(step.wait_ms == 30000);
    step = schedule::scheduler_step(at(1000, 1010, kNever), limits);
    CHECK(step.wait_ms == 10000);
}

TEST_CASE("scheduler step: awake after the boot and after use, not on mains")
{
    const StepLimits limits = battery_limits();
    StepInput input = at(1000, 9000, kNever);
    input.awake_until_boot = 1010;
    Step step = schedule::scheduler_step(input, limits);
    CHECK(step.action == Step::Action::wait);
    CHECK(step.wait_ms == 10000);

    input = at(1000, 9000, kNever);
    input.last_activity = 900;  // + 300 s
    step = schedule::scheduler_step(input, limits);
    CHECK(step.action == Step::Action::wait);

    input = at(1000, 9000, kNever);
    input.sleeps = no;
    step = schedule::scheduler_step(input, limits);
    CHECK(step.action == Step::Action::wait);
    CHECK(step.wait_ms == 30000);

    StepLimits mains = limits;
    mains.deep_sleep = false;
    step = schedule::scheduler_step(at(1000, 9000, kNever), mains);
    CHECK(step.action == Step::Action::wait);
}
