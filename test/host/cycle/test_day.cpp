// A battery hub over a virtual day (#446): the scheduler's decisions
// (scheduler_step) with the real schedule_state, deep sleeps included. The
// cycles themselves succeed at once here; the camera and backend parts
// follow in the later scenarios.
#include <doctest/doctest.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "host_cycle.hpp"
#include "hub_config.hpp"
#include "schedule_state.hpp"
#include "scheduler_step.hpp"

namespace
{

// 2026-06-01 00:00 UTC, a Monday.
constexpr std::int64_t kMidnight = 1780272000;

std::string clock_of(std::int64_t unix_s)
{
    const time_t t = (time_t)unix_s;
    struct tm utc = {};
    gmtime_r(&t, &utc);
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", utc.tm_hour, utc.tm_min);
    return text;
}

bool sleeps(void *)
{
    return schedule::effective().sleeps;
}

// Runs the hub from `start` for `hours`, like scheduler_task with deep sleep
// (CONFIG defaults): boots, decisions, one-minute cycles. Returns the cycles
// as "HH:MM camera|checkin" and counts the deep sleeps.
std::vector<std::string> run_hub(std::int64_t start, int hours, int &sleeps_out)
{
    schedule::StepLimits limits;
    limits.deep_sleep = true;
    limits.awake_after_activity_s = 5 * 60;
    limits.min_sleep_s = 60;
    limits.max_sleep_s = 720 * 60;

    std::vector<std::string> cycles;
    sleeps_out = 0;
    const std::int64_t end = start + hours * 3600LL;
    bool woke = schedule::woke_from_sleep();
    std::int64_t boot = schedule::now();
    while (host_cycle::wall_unix() < end) {
        schedule::StepInput input;
        input.now = schedule::now();
        input.times = schedule::next_times();
        input.start_at = woke ? boot : boot + 60;
        input.awake_until_boot = woke ? boot : boot + 10 * 60;
        input.last_activity = schedule::last_activity();
        input.sleeps = sleeps;
        const schedule::Step step = schedule::scheduler_step(input, limits);
        switch (step.action) {
        case schedule::Step::Action::camera:
        case schedule::Step::Action::checkin: {
            const bool camera = step.action == schedule::Step::Action::camera;
            cycles.push_back(clock_of(host_cycle::wall_unix()) +
                             (camera ? " camera" : " checkin"));
            schedule::on_cycle_started(camera);
            host_cycle::advance_ms(60 * 1000);
            schedule::CycleEnd result;
            result.camera_ok = camera;
            result.sync_attempted = true;
            result.sync_ok = true;
            schedule::on_cycle_finished(result);
            break;
        }
        case schedule::Step::Action::sleep:
            schedule::prepare_sleep(step.sleep_s);
            host_cycle::deep_sleep(step.sleep_s);
            schedule::init();
            woke = schedule::woke_from_sleep();
            boot = schedule::now();
            ++sleeps_out;
            break;
        case schedule::Step::Action::wait:
            host_cycle::advance_ms(step.wait_ms);
            break;
        }
    }
    return cycles;
}

std::string joined(const std::vector<std::string> &lines)
{
    std::string text;
    for (const auto &line : lines) {
        text += line + "\n";
    }
    return text;
}

void battery_hub(bool uplink)
{
    setenv("TZ", "UTC", 1);
    tzset();
    host_cycle::power_on(kMidnight + 8 * 3600);
    REQUIRE(hub_config::set(hub_config::Key::power, "battery", nullptr) ==
            ESP_OK);
    if (uplink) {
        REQUIRE(hub_config::set(hub_config::Key::wifi_ssid, "Hof", nullptr) ==
                ESP_OK);
        REQUIRE(hub_config::set(hub_config::Key::wifi_password, "geheim123",
                                nullptr) == ESP_OK);
        REQUIRE(hub_config::set(hub_config::Key::api_url,
                                "https://api.example.test", nullptr) == ESP_OK);
        REQUIRE(hub_config::set(hub_config::Key::device_key,
                                "hub1.0123456789abcdef0123456789",
                                nullptr) == ESP_OK);
    }
    schedule::init();
}

void check_quiet_hours(const std::vector<std::string> &cycles)
{
    for (const auto &cycle : cycles) {
        // Quiet hours 23:00-06:00: no cycle in them.
        const int hour = std::stoi(cycle.substr(0, 2));
        CHECK_MESSAGE((hour >= 6 && hour < 23), cycle);
    }
}

}  // namespace

TEST_CASE("day: a battery hub without uplink reads every 2 h and sleeps")
{
    battery_hub(false);
    int sleeps = 0;
    const std::vector<std::string> cycles =
        run_hub(kMidnight + 8 * 3600, 24, sleeps);
    MESSAGE(joined(cycles));
    CHECK(joined(cycles) == "08:01 camera\n10:01 camera\n12:01 camera\n"
                            "14:01 camera\n16:01 camera\n18:01 camera\n"
                            "20:01 camera\n22:01 camera\n06:00 camera\n");
    // Between two reads and through the quiet hours (up to 12 h at once).
    CHECK(sleeps == 9);
    check_quiet_hours(cycles);
}

TEST_CASE("day: with a backend the hub also checks in between the reads")
{
    battery_hub(true);
    int sleeps = 0;
    const std::vector<std::string> cycles =
        run_hub(kMidnight + 8 * 3600, 24, sleeps);
    MESSAGE(joined(cycles));
    // Reads every 2 h as before; check-ins every 30 min in between; the last
    // one before the quiet hours at 22:31, the first read after them at 06:00.
    REQUIRE(cycles.size() == 34);
    CHECK(cycles[0] == "08:01 camera");
    CHECK(cycles[1] == "08:31 checkin");
    CHECK(cycles[4] == "10:01 camera");
    CHECK(cycles[29] == "22:31 checkin");
    CHECK(cycles[30] == "06:00 camera");
    CHECK(cycles[33] == "07:30 checkin");
    // After every cycle (the hub is never kept awake here).
    CHECK(sleeps == 34);
    check_quiet_hours(cycles);
}
