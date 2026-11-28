// The whole hub cycle on the host (#446), phase 1: the hub's own state
// (schedule_state with hub_config and NVS) across restarts and deep sleep.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "hub_config.hpp"
#include "host_cycle.hpp"
#include "schedule_state.hpp"

namespace
{

// 2026-06-01 10:00 UTC.
constexpr std::int64_t kStart = 1780308000;

bool has_key(const std::string &key)
{
    const auto keys = host_cycle::nvs_keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// A new battery hub (power set in the installer), booted.
void battery_hub()
{
    host_cycle::power_on(kStart);
    REQUIRE(hub_config::set(hub_config::Key::power, "battery", nullptr) ==
            ESP_OK);
    schedule::init();
}

struct Utc {
    Utc()
    {
        setenv("TZ", "UTC", 1);
        tzset();
    }
};

}  // namespace

TEST_CASE("cycle: a fresh hub uses the built-in schedule")
{
    Utc utc;
    host_cycle::power_on(kStart);
    schedule::init();
    CHECK_FALSE(schedule::woke_from_sleep());
    const schedule::Effective effective = schedule::effective();
    // Mains power by default: someone on site starts the reads, no quiet
    // hours, no deep sleep.
    CHECK(effective.camera_plan == "off");
    CHECK(effective.checkin_min == 30);
    CHECK(effective.quiet == "off");
    CHECK_FALSE(effective.sleeps);

    // Battery power (installer or console) is stored in NVS.
    CHECK(hub_config::set(hub_config::Key::power, "battery", nullptr) ==
          ESP_OK);
    CHECK(has_key("hub_config/power"));
    const schedule::Effective battery = schedule::effective();
    CHECK(battery.camera_plan == "*=120");
    CHECK(battery.camera_plan_origin == schedule::Origin::built_in);
    CHECK(battery.quiet == "23:00-06:00");
    CHECK(battery.sleeps);
}

TEST_CASE("cycle: a plan from the gallery survives a restart, not a new flash")
{
    Utc utc;
    battery_hub();
    schedule::Remote remote;
    remote.has_camera_plan = true;
    remote.camera_plan = "*=60";
    remote.checkin_interval_s = 3600;
    CHECK(schedule::apply_remote(remote));

    host_cycle::restart();
    schedule::init();
    CHECK_FALSE(schedule::woke_from_sleep());
    schedule::Effective effective = schedule::effective();
    CHECK(effective.camera_plan == "*=60");
    CHECK(effective.camera_plan_origin == schedule::Origin::remote);
    CHECK(effective.checkin_min == 60);
    CHECK(effective.checkin_origin == schedule::Origin::remote);

    battery_hub();
    effective = schedule::effective();
    CHECK(effective.camera_plan == "*=120");
    CHECK(effective.camera_plan_origin == schedule::Origin::built_in);
}

TEST_CASE("cycle: an invalid plan from the backend is ignored")
{
    Utc utc;
    battery_hub();
    schedule::Remote remote;
    remote.has_camera_plan = true;
    remote.camera_plan = "not a plan";
    CHECK_FALSE(schedule::apply_remote(remote));
    CHECK(schedule::effective().camera_plan == "*=120");
}

TEST_CASE("cycle: the state of the last cycle survives deep sleep")
{
    Utc utc;
    host_cycle::power_on(kStart);
    schedule::init();
    host_cycle::advance_ms(5000);
    const std::int64_t started = schedule::now();
    schedule::on_cycle_started(true);
    host_cycle::advance_ms(60000);
    schedule::CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    schedule::on_cycle_finished(end);
    const schedule::State before = schedule::state();
    CHECK(before.last_camera == started);
    CHECK(before.last_checkin == started);
    const std::int64_t asleep_at = schedule::now();

    schedule::prepare_sleep(3600);
    host_cycle::deep_sleep(3600);
    schedule::init();
    CHECK(schedule::woke_from_sleep());
    // The schedule clock moved on by the sleep, the state is the same.
    CHECK(schedule::now() == asleep_at + 3600);
    CHECK(schedule::to_unix(schedule::now()) == host_cycle::wall_unix());
    CHECK(schedule::state().last_camera == started);
    CHECK(schedule::state().last_checkin == started);
    CHECK(schedule::state().cycle_start == schedule::kNever);

    // A restart (not a wake-up) starts afresh.
    host_cycle::restart();
    schedule::init();
    CHECK_FALSE(schedule::woke_from_sleep());
    CHECK(schedule::state().last_camera != started);
}

TEST_CASE("cycle: a GNSS fix is kept in NVS")
{
    Utc utc;
    host_cycle::power_on(kStart);
    schedule::init();
    schedule::set_gnss_location(52.52, 13.40);
    CHECK(has_key("schedule/gnss_loc"));
    host_cycle::restart();
    schedule::init();
    const schedule::Effective effective = schedule::effective();
    CHECK(effective.location_origin == schedule::Origin::gnss);
    CHECK(effective.location == "52.5,13.4");
}
