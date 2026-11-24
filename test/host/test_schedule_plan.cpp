#include <doctest/doctest.h>

#include <string>
#include <type_traits>

#include "schedule_plan.hpp"

using namespace schedule;

namespace
{

constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kDay = 24 * kHour;

// Schedule seconds 0 are local midnight; the clock holds the time.
int local_minute(std::int64_t t)
{
    return (int)(((t % kDay) + kDay) % kDay / 60);
}

const LocalMinute kLocal = local_minute;
const LocalMinute kNoClock;

Plan plan_of(const std::string &text)
{
    Plan plan;
    std::string error;
    REQUIRE_MESSAGE(parse_plan(text, plan, &error), error);
    return plan;
}

std::int64_t at(int hours, int minutes = 0, int day = 0)
{
    return day * kDay + hours * kHour + minutes * 60;
}

Settings settings_with(const std::string &plan, std::uint32_t checkin_min)
{
    Settings settings;
    settings.camera = plan_of(plan);
    settings.checkin_min = checkin_min;
    settings.uplink = true;
    settings.low_battery_percent = 20;
    settings.low_battery_factor = 3;
    return settings;
}

}  // namespace

TEST_CASE("the RTC state is plain data")
{
    CHECK(std::is_trivially_copyable<State>::value);
    CHECK(std::is_trivially_copyable<ReadRequests>::value);
}

TEST_CASE("plans are parsed and formatted")
{
    Plan plan = plan_of("05:00-09:00=30, 17:00-22:00=30 *=120");
    REQUIRE(plan.windows.size() == 2);
    CHECK(plan.windows[0].start == 300);
    CHECK(plan.windows[0].end == 540);
    CHECK(plan.windows[0].interval_min == 30);
    CHECK(plan.default_min == 120);
    CHECK(format_plan(plan) == "05:00-09:00=30,17:00-22:00=30,*=120");

    CHECK(format_plan(plan_of("*=120")) == "*=120");
    CHECK(format_plan(plan_of("22:00-06:00=0,*=60")) == "22:00-06:00=0,*=60");
    CHECK(format_plan(plan_of("20:00-24:00=15")) == "20:00-24:00=15,*=0");
    CHECK_FALSE(plan_of("").reads());
    CHECK_FALSE(plan_of("off").reads());
    CHECK_FALSE(plan_of("*=0").reads());
    CHECK(format_plan(plan_of("off")) == "off");
}

TEST_CASE("invalid plans are rejected with a reason")
{
    Plan plan;
    std::string error;
    for (const char *text :
         {"*", "*=abc", "*=1441", "*=-5", "5:00-9:00=30", "05:00=30",
          "05:00-05:00=30", "25:00-26:00=10", "05:60-06:00=10",
          "24:00-01:00=10", "*=10,*=20",
          "01:00-02:00=1,02:00-03:00=1,03:00-04:00=1,04:00-05:00=1,"
          "05:00-06:00=1,06:00-07:00=1,07:00-08:00=1,08:00-09:00=1,"
          "09:00-10:00=1"}) {
        error.clear();
        CHECK_FALSE_MESSAGE(parse_plan(text, plan, &error), text);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("the first matching window gives the interval")
{
    const Plan plan =
        plan_of("05:00-09:00=30,08:00-10:00=10,22:00-02:00=0,*=120");
    CHECK(interval_at(plan, 4 * 60 + 59) == 120);
    CHECK(interval_at(plan, 5 * 60) == 30);
    CHECK(interval_at(plan, 8 * 60 + 30) == 30);
    CHECK(interval_at(plan, 9 * 60) == 10);
    CHECK(interval_at(plan, 10 * 60) == 120);
    CHECK(interval_at(plan, 23 * 60) == 0);
    CHECK(interval_at(plan, 1 * 60) == 0);
    CHECK(interval_at(plan, 2 * 60) == 120);
}

TEST_CASE("without a clock the default or the longest window interval applies")
{
    CHECK(clockless_interval(plan_of("05:00-09:00=30,*=120")) == 120);
    CHECK(clockless_interval(plan_of("05:00-09:00=30,17:00-20:00=45")) == 45);

    const Plan plan = plan_of("05:00-09:00=30,*=120");
    CHECK(next_due(plan, kNever, 1000, 1, kNoClock) == 1000);
    CHECK(next_due(plan, 1000, 2000, 1, kNoClock) == 1000 + 120 * 60);
    CHECK(next_due(plan, 1000, 9000, 1, kNoClock) == 9000);
    CHECK(next_due(plan, 1000, 2000, 2, kNoClock) == 1000 + 240 * 60);
    CHECK(next_due(plan_of("off"), kNever, 1000, 1, kNoClock) == kNever);
}

TEST_CASE("reads follow the interval of the moment")
{
    const Plan plan = plan_of("05:00-09:00=30,17:00-22:00=30,*=120");
    // First read: at once.
    CHECK(next_due(plan, kNever, at(12), 1, kLocal) == at(12));
    // Midday: two hours after the last read.
    CHECK(next_due(plan, at(12), at(12, 5), 1, kLocal) == at(14));
    // The evening window starts before the two hours are over.
    CHECK(next_due(plan, at(16, 10), at(16, 20), 1, kLocal) == at(17));
    // Inside the window: every 30 minutes, not on minute boundaries only.
    CHECK(next_due(plan, at(17, 0) + 17, at(17, 1), 1, kLocal) ==
          at(17, 30) + 17);
    // Over midnight into the next morning window.
    CHECK(next_due(plan, at(23, 30), at(23, 31), 1, kLocal) == at(1, 30, 1));
    // Overdue: now.
    CHECK(next_due(plan, at(1), at(9, 30), 1, kLocal) == at(9, 30));
}

TEST_CASE("windows without reads are skipped")
{
    const Plan plan = plan_of("21:00-06:00=0,*=60");
    CHECK(next_due(plan, at(20, 30), at(20, 31), 1, kLocal) == at(6, 0, 1));
    const Plan only_evening = plan_of("18:00-20:00=20");
    CHECK(next_due(only_evening, at(19, 50), at(19, 55), 1, kLocal) ==
          at(18, 0, 1));
    CHECK(next_due(only_evening, kNever, at(10), 1, kLocal) == at(18));
}

TEST_CASE("the factor stretches the interval up to a day")
{
    const Plan plan = plan_of("*=120");
    CHECK(next_due(plan, at(0), at(0, 1), 4, kLocal) == at(8));
    CHECK(next_due(plan, at(0), at(0, 1), 16, kLocal) == at(0, 0, 1));
    CHECK(stretched(120, 1) == 120);
    CHECK(stretched(120, 16) == 1440);
    CHECK(stretched(1440, 4) == 1440);
    CHECK(backoff_factor(0) == 1);
    CHECK(backoff_factor(3) == 8);
    CHECK(backoff_factor(10) == 16);
}

TEST_CASE("a local clock that jumps (summer time) is followed")
{
    // Local time is an hour ahead from schedule second at(2) on.
    const LocalMinute dst = [](std::int64_t t) {
        return local_minute(t >= at(2) ? t + kHour : t);
    };
    const Plan plan = plan_of("05:00-07:00=10,*=600");
    // 05:00 local is reached at schedule time 04:00.
    CHECK(next_due(plan, at(0), at(1), 1, dst) == at(4));
}

TEST_CASE("camera reads and check-ins")
{
    Settings settings = settings_with("*=120", 30);
    State state = initial_state();

    Times times = next_times(settings, state, at(8), kLocal);
    CHECK(times.camera == at(8));
    CHECK(times.checkin == at(8));
    CHECK(due(times, at(8)) == Task::camera);

    // A scan reads the camera and syncs.
    cycle_started(state, true, at(8));
    CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    times = next_times(settings, state, at(8, 2), kLocal);
    CHECK(times.camera == at(10));
    CHECK(times.checkin == at(8, 30));
    CHECK(due(times, at(8, 2)) == Task::none);
    CHECK(due(times, at(8, 30)) == Task::checkin);

    // An upload only moves the check-in.
    cycle_started(state, false, at(8, 30));
    end = CycleEnd{};
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    times = next_times(settings, state, at(8, 31), kLocal);
    CHECK(times.camera == at(10));
    CHECK(times.checkin == at(9));
}

TEST_CASE("no check-ins without uplink settings or interval")
{
    Settings settings = settings_with("*=120", 30);
    settings.uplink = false;
    CHECK(next_times(settings, initial_state(), 0, kLocal).checkin == kNever);
    settings.uplink = true;
    settings.checkin_min = 0;
    CHECK(next_times(settings, initial_state(), 0, kLocal).checkin == kNever);
}

TEST_CASE("failed camera reads back off, a good read resets")
{
    Settings settings = settings_with("*=60", 0);
    State state = initial_state();
    CycleEnd failed;
    failed.camera_failed = true;

    cycle_started(state, true, at(0));
    cycle_finished(state, failed);
    CHECK(state.camera_failures == 1);
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == at(2));

    cycle_started(state, true, at(2));
    cycle_finished(state, failed);
    CHECK(next_times(settings, state, at(2, 1), kLocal).camera == at(6));

    CycleEnd ok;
    ok.camera_ok = true;
    cycle_started(state, true, at(6));
    cycle_finished(state, ok);
    CHECK(state.camera_failures == 0);
    CHECK(next_times(settings, state, at(6, 1), kLocal).camera == at(7));
}

TEST_CASE("a stopped cycle neither counts as failure nor as sync")
{
    Settings settings = settings_with("*=60", 30);
    State state = initial_state();
    cycle_started(state, true, at(1));
    cycle_finished(state, CycleEnd{});
    CHECK(state.camera_failures == 0);
    CHECK(state.last_camera == at(1));
    CHECK(state.last_checkin == kNever);
    CHECK(state.cycle_start == kNever);
}

TEST_CASE("failed syncs back off the check-ins")
{
    Settings settings = settings_with("off", 30);
    State state = initial_state();
    CycleEnd failed;
    failed.sync_attempted = true;
    cycle_started(state, false, at(0));
    cycle_finished(state, failed);
    CHECK(state.checkin_failures == 1);
    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(1));
    cycle_started(state, false, at(1));
    cycle_finished(state, failed);
    CHECK(next_times(settings, state, at(1, 1), kLocal).checkin == at(3));
}

TEST_CASE("a camera with a low battery is read less often")
{
    Settings settings = settings_with("*=60", 0);
    State state = initial_state();
    state.last_camera = at(0);
    state.camera_battery = 50;
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == at(1));
    state.camera_battery = 19;
    CHECK(camera_factor(settings, state) == 3);
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == at(3));
    settings.low_battery_percent = 0;
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == at(1));
}

namespace
{

PowerSettings power_settings_for_tests()
{
    PowerSettings power;
    power.low_percent = 30;
    power.critical_percent = 10;
    power.critical_mv = 3500;
    power.hysteresis_percent = 5;
    power.hysteresis_mv = 50;
    power.low_camera_factor = 3;
    power.low_checkin_factor = 2;
    power.critical_checkin_min = 720;
    return power;
}

}  // namespace

TEST_CASE("the hub battery level follows its thresholds with hysteresis")
{
    const PowerSettings power = power_settings_for_tests();
    const PowerLevel normal = PowerLevel::normal;
    const PowerLevel low = PowerLevel::low;
    const PowerLevel critical = PowerLevel::critical;

    CHECK(power_level(power, 80, 4000, normal) == normal);
    CHECK(power_level(power, 30, 3800, normal) == normal);
    CHECK(power_level(power, 29, 3800, normal) == low);
    // Low is left only at 35 %.
    CHECK(power_level(power, 32, 3800, low) == low);
    CHECK(power_level(power, 35, 3800, low) == normal);

    CHECK(power_level(power, 9, 3700, low) == critical);
    // A low voltage is critical whatever the percentage.
    CHECK(power_level(power, 60, 3499, normal) == critical);
    // Critical is left at 15 % and 3550 mV, then into low.
    CHECK(power_level(power, 14, 3700, critical) == critical);
    CHECK(power_level(power, 20, 3520, critical) == critical);
    CHECK(power_level(power, 15, 3550, critical) == low);
    CHECK(power_level(power, 40, 3600, critical) == normal);

    // Unknown battery: normal; one known value counts alone.
    CHECK(power_level(power, -1, -1, critical) == normal);
    CHECK(power_level(power, -1, 3400, normal) == critical);
    CHECK(power_level(power, 20, -1, normal) == low);

    // Thresholds off.
    PowerSettings off;
    CHECK(power_level(off, 1, 3000, normal) == normal);
}

TEST_CASE("a low hub battery stretches camera reads and check-ins")
{
    Settings settings = settings_with("*=60", 30);
    settings.power = power_settings_for_tests();
    State state = initial_state();
    state.last_camera = at(0);
    state.last_checkin = at(0);

    Times times = next_times(settings, state, at(0, 1), kLocal);
    CHECK(times.camera == at(1));
    CHECK(times.checkin == at(0, 30));
    CHECK(stretch(settings, state) == Stretch::none);

    CHECK(set_hub_battery(state, settings.power, 25, 3700, false));
    CHECK(hub_level(state) == PowerLevel::low);
    times = next_times(settings, state, at(0, 1), kLocal);
    CHECK(times.camera == at(3));
    CHECK(times.checkin == at(1));
    CHECK(stretch(settings, state) == Stretch::hub_battery_low);
    CHECK(std::string(stretch_name(stretch(settings, state))) ==
          "hubBatteryLow");

    // Both batteries low: the factors multiply, the hub is named.
    state.camera_battery = 10;
    CHECK(camera_factor(settings, state) == 9);
    CHECK(stretch(settings, state) == Stretch::hub_battery_low);
    state.camera_battery = -1;

    // While the cell charges the low level does not stretch.
    CHECK_FALSE(set_hub_battery(state, settings.power, 25, 3700, true));
    CHECK(hub_level(state) == PowerLevel::low);
    times = next_times(settings, state, at(0, 1), kLocal);
    CHECK(times.camera == at(1));
    CHECK(times.checkin == at(0, 30));
    CHECK(stretch(settings, state) == Stretch::none);
}

TEST_CASE("a critical hub battery stops camera reads and checks in rarely")
{
    Settings settings = settings_with("*=60", 30);
    settings.power = power_settings_for_tests();
    State state = initial_state();
    state.last_camera = at(0);
    state.last_checkin = at(0);
    state.checkin_failures = 3;

    CHECK(set_hub_battery(state, settings.power, 8, 3600, true));
    CHECK(hub_level(state) == PowerLevel::critical);
    Times times = next_times(settings, state, at(0, 1), kLocal);
    CHECK(times.camera == kNever);
    // 12 h, no back-off on top; also while charging.
    CHECK(times.checkin == at(12));
    CHECK(stretch(settings, state) == Stretch::hub_battery_critical);
    CHECK(std::string(stretch_name(stretch(settings, state))) ==
          "hubBatteryCritical");

    // A read asked for by the backend waits until the level is left.
    state.camera_requested = true;
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == kNever);
    CHECK(set_hub_battery(state, settings.power, 50, 3900, false));
    CHECK(next_times(settings, state, at(0, 1), kLocal).camera == at(0, 1));

    // The first check-in after a power-on is due at once.
    State fresh = initial_state();
    set_hub_battery(fresh, settings.power, 5, 3400, false);
    CHECK(next_times(settings, fresh, at(0), kLocal).checkin == at(0));

    // A normal check-in interval above the critical one is kept.
    settings.checkin_min = 1440;
    set_hub_battery(state, settings.power, 5, 3400, false);
    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(24));
}

TEST_CASE("an unknown hub battery changes nothing")
{
    Settings settings = settings_with("*=60", 30);
    settings.power = power_settings_for_tests();
    State state = initial_state();
    CHECK(hub_level(state) == PowerLevel::normal);
    CHECK_FALSE(set_hub_battery(state, settings.power, -1, -1, false));
    CHECK(stretch(settings, state) == Stretch::none);
    CHECK(std::string(stretch_name(Stretch::none)).empty());
}

TEST_CASE("a requested read is due at once, also with reads off")
{
    Settings settings = settings_with("off", 30);
    State state = initial_state();
    state.last_checkin = at(0);
    CHECK(next_times(settings, state, at(0, 5), kLocal).camera == kNever);
    state.camera_requested = true;
    const Times times = next_times(settings, state, at(0, 5), kLocal);
    CHECK(due(times, at(0, 5)) == Task::camera);
    cycle_started(state, true, at(0, 5));
    CHECK_FALSE(state.camera_requested);
}

TEST_CASE("the check-in reports the times after a successful sync")
{
    Settings settings = settings_with("*=120", 30);
    State state = initial_state();
    state.camera_failures = 2;
    state.checkin_failures = 3;
    cycle_started(state, true, at(10));
    const State next = after_successful_sync(state);
    const Times times = next_times(settings, next, at(10, 3), kLocal);
    CHECK(times.camera == at(12));
    CHECK(times.checkin == at(10, 30));
    // The running state is unchanged.
    CHECK(state.cycle_start == at(10));
    CHECK(state.checkin_failures == 3);
}

TEST_CASE("readCamera commands are answered after the next read")
{
    ReadRequests requests;
    requests.clear();
    bool is_new = false;
    CHECK(requests.on_command("cmd-1", "CAM1", &is_new) ==
          ReadRequests::Reply::pending);
    CHECK(is_new);
    CHECK(requests.any_pending());
    // Sent again before the read: still pending, nothing new to do.
    CHECK(requests.on_command("cmd-1", "CAM1", &is_new) ==
          ReadRequests::Reply::pending);
    CHECK_FALSE(is_new);

    requests.on_camera_read("CAM1");
    requests.on_camera_part_ended();
    CHECK_FALSE(requests.any_pending());
    CHECK(requests.on_command("cmd-1", "CAM1", &is_new) ==
          ReadRequests::Reply::done);
    CHECK_FALSE(is_new);
    // Forgotten once answered.
    CHECK(requests.on_command("cmd-1", "CAM1", &is_new) ==
          ReadRequests::Reply::pending);
    CHECK(is_new);

    requests.on_camera_part_ended();
    CHECK(requests.on_command("cmd-1", "CAM1", &is_new) ==
          ReadRequests::Reply::failed);
}

TEST_CASE("a readCamera command is done only when its camera was read")
{
    ReadRequests requests;
    requests.clear();
    bool is_new = false;
    requests.on_command("for-1", "CAM1", &is_new);
    requests.on_command("for-2", "CAM2", &is_new);

    // The scan read only the other camera.
    requests.on_camera_read("CAM2");
    CHECK(requests.any_pending());
    requests.on_camera_part_ended();
    CHECK(requests.on_command("for-1", "CAM1", &is_new) ==
          ReadRequests::Reply::failed);
    CHECK(requests.on_command("for-2", "CAM2", &is_new) ==
          ReadRequests::Reply::done);

    // A camera id that does not fit is any camera.
    const std::string long_cam(ReadRequests::kCamIdSize, 'c');
    requests.on_command("any", long_cam, &is_new);
    requests.on_camera_read("CAM9");
    CHECK(requests.on_command("any", long_cam, &is_new) ==
          ReadRequests::Reply::done);
}

TEST_CASE("readCamera commands beyond the table still ask for a read")
{
    ReadRequests requests;
    requests.clear();
    bool is_new = false;
    for (int i = 0; i < 6; i++) {
        CHECK(requests.on_command("cmd-" + std::to_string(i), "CAM1",
                                  &is_new) == ReadRequests::Reply::pending);
        CHECK(is_new);
    }
    const std::string long_id(ReadRequests::kIdSize, 'x');
    CHECK(requests.on_command(long_id, "CAM1", &is_new) ==
          ReadRequests::Reply::pending);
    CHECK(is_new);
}

TEST_CASE("settings come from the backend, the hub or the build")
{
    const std::string remote = "*=30";
    const std::string local = "*=60";
    const std::string empty;
    const std::string built_in = "*=120";
    Origin origin = Origin::built_in;
    CHECK(pick(&remote, &local, built_in, &origin) == remote);
    CHECK(origin == Origin::remote);
    CHECK(pick(&empty, &local, built_in, &origin) == local);
    CHECK(origin == Origin::local);
    CHECK(pick(nullptr, &empty, built_in, &origin) == built_in);
    CHECK(origin == Origin::built_in);
    CHECK(std::string(origin_name(Origin::remote)) == "remote");
}

TEST_CASE("checkin_max_min bounds the back-off of check-ins")
{
    Settings settings = settings_with("*=60", 240);
    State state = initial_state();
    state.last_camera = at(0);
    state.last_checkin = at(0);
    state.checkin_failures = 3;  // 4 h x 8 = 32 h, kMaxStretchedMin: 24 h

    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(24));

    settings.checkin_max_min = 12 * 60;
    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(12));

    // Without failures the interval itself applies.
    state.checkin_failures = 0;
    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(4));

    // A critical hub battery is not bounded by it.
    settings.power = power_settings_for_tests();
    settings.power.critical_checkin_min = 24 * 60;
    CHECK(set_hub_battery(state, settings.power, 8, 3600, false));
    CHECK(next_times(settings, state, at(0, 1), kLocal).checkin == at(24));
}

namespace
{

Quiet quiet_of(const std::string &text)
{
    Quiet quiet;
    std::string error;
    REQUIRE_MESSAGE(parse_quiet(text, quiet, &error), error);
    return quiet;
}

}  // namespace

TEST_CASE("quiet hours are parsed and formatted")
{
    CHECK(format_quiet(quiet_of("23:00-06:00")) == "23:00-06:00");
    CHECK(format_quiet(quiet_of(" 22:30 - 05:15 ")) == "22:30-05:15");
    CHECK_FALSE(quiet_of("off").enabled());
    CHECK_FALSE(quiet_of("").enabled());
    CHECK(format_quiet(quiet_of("")) == "off");

    Quiet quiet;
    std::string error;
    CHECK_FALSE(parse_quiet("23:00", quiet, &error));
    CHECK_FALSE(parse_quiet("25:00-06:00", quiet, &error));
    CHECK_FALSE(parse_quiet("06:00-06:00", quiet, &error));
    CHECK_FALSE(parse_quiet("24:00-06:00", quiet, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("quiet_end finds the end of the window, also across midnight")
{
    const Quiet night = quiet_of("23:00-06:00");
    CHECK_FALSE(in_quiet(night, at(12), kLocal));
    CHECK(in_quiet(night, at(23), kLocal));
    CHECK(in_quiet(night, at(3), kLocal));
    CHECK_FALSE(in_quiet(night, at(6), kLocal));
    CHECK(quiet_end(night, at(12), kLocal) == at(12));
    CHECK(quiet_end(night, at(23, 30), kLocal) == at(6, 0, 1));
    CHECK(quiet_end(night, at(2), kLocal) == at(6));
    // Without a clock the quiet hours do not apply.
    CHECK_FALSE(in_quiet(night, at(3), kNoClock));
    CHECK(quiet_end(night, at(3), kNoClock) == at(3));
    // A window inside one day.
    const Quiet noon = quiet_of("12:00-14:00");
    CHECK(quiet_end(noon, at(12, 30), kLocal) == at(14));
}

TEST_CASE("nothing is due in quiet hours; it runs at their end")
{
    Settings settings = settings_with("*=120", 30);
    settings.quiet = quiet_of("23:00-06:00");
    State state = initial_state();

    // The last cycle was at 22:30: the check-in would be due at 23:00, the
    // read at 00:30.
    cycle_started(state, true, at(22, 30));
    CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);

    Times times = next_times(settings, state, at(22, 31), kLocal);
    CHECK(times.camera == at(6, 0, 1));
    CHECK(times.checkin == at(6, 0, 1));
    CHECK(due(times, at(3)) == Task::none);
    // At the end of the night one cycle with a read (it syncs as well).
    CHECK(due(times, at(6, 0, 1)) == Task::camera);

    // During the day nothing changes.
    cycle_started(state, true, at(6));
    cycle_finished(state, end);
    times = next_times(settings, state, at(6, 1), kLocal);
    CHECK(times.checkin == at(6, 30));
    CHECK(times.camera == at(8));
}

TEST_CASE("a requested read waits for the end of the quiet hours")
{
    Settings settings = settings_with("*=120", 30);
    settings.quiet = quiet_of("23:00-06:00");
    State state = initial_state();
    cycle_started(state, true, at(22));
    CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    state.camera_requested = true;

    CHECK(next_times(settings, state, at(2), kLocal).camera == at(6));
    CHECK(next_times(settings, state, at(21), kLocal).camera == at(21));
}

TEST_CASE("without a clock or with the window off nothing is held back")
{
    Settings settings = settings_with("*=120", 30);
    settings.quiet = quiet_of("23:00-06:00");
    State state = initial_state();
    CHECK(next_times(settings, state, at(2), kNoClock).camera == at(2));
    settings.quiet = quiet_of("off");
    CHECK(next_times(settings, state, at(2), kLocal).camera == at(2));
}

namespace
{

// Sunrise 04:45, sunset 21:30 (a June day) or 08:15 / 15:55 (December).
const SunMinutes kSummerSun = [](std::int64_t) {
    SunDay day;
    day.sunrise = 4 * 60 + 45;
    day.sunset = 21 * 60 + 30;
    return day;
};
const SunMinutes kWinterSun = [](std::int64_t) {
    SunDay day;
    day.sunrise = 8 * 60 + 15;
    day.sunset = 15 * 60 + 55;
    return day;
};

}  // namespace

TEST_CASE("plans with sun windows are parsed and formatted")
{
    const std::string text =
        "sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120";
    Plan plan = plan_of(text);
    REQUIRE(plan.windows.size() == 2);
    CHECK(plan.windows[0].start_sun == 1);
    CHECK(plan.windows[0].start_offset == -60);
    CHECK(plan.windows[0].end_sun == 1);
    CHECK(plan.windows[0].end_offset == 90);
    CHECK(plan.windows[1].start_sun == 2);
    CHECK(format_plan(plan) == text);
    // Mixed with clock bounds and the older form.
    plan = plan_of("05:00..sunrise+30=15,sunset..23:00=20,17:00-18:00=5");
    CHECK(format_plan(plan) ==
          "05:00..sunrise+30=15,sunset..23:00=20,17:00-18:00=5,*=0");
    // No sign is no offset.
    CHECK(format_plan(plan_of("sunrise..sunset=60")) ==
          "sunrise..sunset=60,*=0");

    Plan bad;
    std::string error;
    CHECK_FALSE(parse_plan("sunrise-60..noon=30", bad, &error));
    CHECK_FALSE(parse_plan("sunrise*5..sunset=30", bad, &error));
    CHECK_FALSE(parse_plan("sunrise-9999..sunset=30", bad, &error));
    CHECK_FALSE(parse_plan("sunrise..sunrise=30", bad, &error));
    CHECK_FALSE(parse_plan("sunrise-60..", bad, &error));
    CHECK_FALSE(parse_plan("24:00..sunrise=30", bad, &error));
}

TEST_CASE("sun windows follow the sun through the year")
{
    const Plan plan = plan_of("sunrise-60..sunrise+90=30,*=120");
    SunDay summer;
    summer.sunrise = 4 * 60 + 45;
    summer.sunset = 21 * 60 + 30;
    SunDay winter;
    winter.sunrise = 8 * 60 + 15;
    winter.sunset = 15 * 60 + 55;
    // Summer: 03:45-06:15.
    CHECK(interval_at(plan, 3 * 60 + 44, summer) == 120);
    CHECK(interval_at(plan, 3 * 60 + 45, summer) == 30);
    CHECK(interval_at(plan, 6 * 60 + 14, summer) == 30);
    CHECK(interval_at(plan, 6 * 60 + 15, summer) == 120);
    // Winter: 07:15-09:45.
    CHECK(interval_at(plan, 5 * 60, winter) == 120);
    CHECK(interval_at(plan, 7 * 60 + 15, winter) == 30);
    // Unknown sun times: the window is skipped.
    CHECK(interval_at(plan, 5 * 60, SunDay()) == 120);
    CHECK(interval_at(plan, 5 * 60) == 120);
}

TEST_CASE("a sun window that spans midnight is followed")
{
    // Sunset - 30 to 01:00.
    const Plan plan = plan_of("sunset-30..01:00=10,*=60");
    SunDay day;
    day.sunrise = 5 * 60;
    day.sunset = 22 * 60;
    CHECK(interval_at(plan, 21 * 60 + 29, day) == 60);
    CHECK(interval_at(plan, 21 * 60 + 30, day) == 10);
    CHECK(interval_at(plan, 30, day) == 10);
    CHECK(interval_at(plan, 60, day) == 60);
}

TEST_CASE("reads follow the sun windows")
{
    Settings settings = settings_with("sunrise-60..sunrise+90=30,*=0", 0);
    settings.uplink = false;
    State state = initial_state();
    cycle_started(state, true, at(0));
    CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    // Summer: the first read of the day at 03:45, the window's start.
    CHECK(next_times(settings, state, at(1), kLocal, kSummerSun).camera ==
          at(3, 45));
    // Winter: at 07:15.
    CHECK(next_times(settings, state, at(1), kLocal, kWinterSun).camera ==
          at(7, 15));
}

TEST_CASE("the quiet hours end with the morning sun window in summer")
{
    Settings settings = settings_with(
        "sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120", 30);
    settings.quiet = quiet_of("23:00-06:00");
    State state = initial_state();
    // The last cycle was in the evening at 22:40: the check-in falls due at
    // 23:10, inside the quiet hours.
    cycle_started(state, true, at(22, 40));
    CycleEnd end;
    end.camera_ok = true;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);

    // Summer: sunrise 04:45, the window opens at 03:45, in the quiet hours.
    Times times = next_times(settings, state, at(22, 41), kLocal, kSummerSun);
    CHECK(times.camera == at(3, 45, 1));
    CHECK(times.checkin == at(3, 45, 1));
    // Winter: sunrise 08:15, so the quiet hours end at 06:00 as set.
    times = next_times(settings, state, at(22, 41), kLocal, kWinterSun);
    CHECK(times.camera == at(6, 0, 1));
    // Without sun times the fixed end holds.
    times = next_times(settings, state, at(22, 41), kLocal);
    CHECK(times.camera == at(6, 0, 1));
}

TEST_CASE("an evening sun window keeps the hub awake past the quiet start")
{
    Plan plan = plan_of("sunset-90..sunset+90=30,*=120");
    const Quiet night = quiet_of("23:00-06:00");
    SunDay day;
    day.sunrise = 4 * 60;
    day.sunset = 22 * 60;  // window until 23:30
    const SunMinutes sun = [day](std::int64_t) { return day; };
    CHECK_FALSE(in_quiet(night, at(23, 10), kLocal, &plan, sun));
    CHECK(in_quiet(night, at(23, 40), kLocal, &plan, sun));
    // The same without a plan and sun: quiet from 23:00.
    CHECK(in_quiet(night, at(23, 10), kLocal));
}

TEST_CASE("the energy factor shortens the check-in interval")
{
    Settings settings = settings_with("*=0", 30);
    settings.checkin_energy_permille = 500;
    State state = initial_state();
    cycle_started(state, false, at(8));
    CycleEnd end;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    CHECK(next_times(settings, state, at(8, 1), kLocal).checkin == at(8, 15));
    settings.checkin_energy_permille = 750;
    // 22.5 minutes, rounded down to 22.
    CHECK(next_times(settings, state, at(8, 1), kLocal).checkin == at(8, 22));
    // Not below the floor.
    settings.checkin_min = 15;
    settings.checkin_energy_permille = 500;
    CHECK(next_times(settings, state, at(8, 1), kLocal).checkin == at(8, 10));
    // An interval shorter than the floor is not raised.
    settings.checkin_min = 5;
    CHECK(next_times(settings, state, at(8, 1), kLocal).checkin == at(8, 5));
}

TEST_CASE("the energy factor does not apply at a low hub battery")
{
    Settings settings = settings_with("*=0", 30);
    settings.power = power_settings_for_tests();
    settings.power.low_checkin_factor = 2;
    settings.checkin_energy_permille = 500;
    State state = initial_state();
    CHECK(set_hub_battery(state, settings.power, 25, 3700, false));
    cycle_started(state, false, at(8));
    CycleEnd end;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(state, end);
    // Low level: the stretched 60 minutes, not shortened.
    CHECK(next_times(settings, state, at(8, 1), kLocal).checkin == at(9));
}
