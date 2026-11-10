#pragma once

// Wake-up schedule of the hub: when to read the cameras and when to check in
// with the backend. Pure logic, host-testable; the state kept across deep
// sleep, the clock and the stored settings live in schedule.cpp, and the task
// that requests the cycles and puts the hub to sleep in the scheduler
// component.
//
// Times are "schedule seconds": a monotonic count that keeps running across
// deep sleep and is not changed when the clock is set (see schedule.hpp).
// Local times of the day are only used for the read windows, and only when
// the clock holds the time.
//
// Camera reads follow a plan of time windows, each with its own interval, and
// an interval for the rest of the day:
//
//   "05:00-09:00=30,17:00-22:00=30,*=120"
//
// reads every 30 minutes from 5 to 9 and from 17 to 22 o'clock and every two
// hours otherwise. A read is due as soon as the time since the last one has
// reached the interval that applies at that moment, so the first read in a
// window comes at its start if the last one is older than the window's
// interval. Without a valid clock the windows are ignored and the "*"
// interval applies (if it is 0 or missing, the longest window interval).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace schedule
{

constexpr std::int64_t kNever = -1;
constexpr std::size_t kMaxWindows = 8;
// Longest interval in a plan and for check-ins (minutes).
constexpr std::uint32_t kMaxIntervalMin = 1440;
// Back-off and the low-battery stretch do not extend an interval beyond this
// (minutes), unless the interval itself is longer.
constexpr std::uint32_t kMaxStretchedMin = 1440;
// How far next_due() looks ahead (seconds).
constexpr std::int64_t kHorizonS = 3 * 24 * 3600;

struct Window {
    // Minutes of the day in local time. `end` is exclusive and may be 1440
    // (24:00); an end before the start spans midnight.
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    // Interval in minutes; 0: no reads in this window.
    std::uint16_t interval_min = 0;
};

struct Plan {
    // The first window that contains a time applies.
    std::vector<Window> windows;
    // Interval outside the windows; 0: no reads.
    std::uint16_t default_min = 0;

    // True if the plan has any read at all.
    bool reads() const;
};

// Parses a plan: entries "HH:MM-HH:MM=MIN" and "*=MIN", separated by commas
// or spaces. "off" or an empty text is a plan without reads. Returns false
// with a short English reason in `error`.
bool parse_plan(const std::string &text, Plan &out, std::string *error);

// The plan in the form parse_plan() reads ("off" for no reads).
std::string format_plan(const Plan &plan);

// Interval in minutes at a local minute of the day (0: no reads).
std::uint32_t interval_at(const Plan &plan, int minute_of_day);

// Interval in minutes when the clock does not hold the time.
std::uint32_t clockless_interval(const Plan &plan);

// Maps schedule seconds to the local minute of the day (0-1439). Empty when
// the clock does not hold the time.
using LocalMinute = std::function<int(std::int64_t)>;

// 1, 2, 4, 8, 16 for 0, 1, 2, 3, 4 or more failures in a row.
std::uint32_t backoff_factor(std::uint32_t failures);

// `interval_min` times `factor`, but not beyond kMaxStretchedMin (unless the
// interval itself is longer).
std::uint32_t stretched(std::uint32_t interval_min, std::uint32_t factor);

// The earliest time t >= now at which t - last has reached the interval (times
// `factor`) that applies at t. `last` is kNever if there was no read yet: then
// any time with an interval counts. kNever if there is none within kHorizonS.
std::int64_t next_due(const Plan &plan, std::int64_t last, std::int64_t now,
                      std::uint32_t factor, const LocalMinute &local);

// --- The hub's own battery ---

// How much the hub saves because of its own battery. Low: camera reads and
// check-ins are stretched (not while the cell charges). Critical: no camera
// reads, only check-ins at a long interval, so the hub still reports its
// battery.
enum class PowerLevel : std::uint8_t {
    normal = 0,
    low = 1,
    critical = 2,
};

struct PowerSettings {
    // Below this percentage the level is low; 0: off.
    int low_percent = 0;
    // Below this percentage, or below critical_mv, it is critical; 0: off.
    int critical_percent = 0;
    int critical_mv = 0;
    // A level is left only this far above its threshold.
    int hysteresis_percent = 5;
    int hysteresis_mv = 50;
    // Low: factors for the camera read and the check-in interval.
    std::uint32_t low_camera_factor = 1;
    std::uint32_t low_checkin_factor = 1;
    // Critical: check-in interval in minutes (at least the normal one).
    std::uint32_t critical_checkin_min = 0;
};

// The level for a reading (`percent`, `millivolts`: -1 unknown), starting
// from the level before (hysteresis). An unknown battery is normal.
PowerLevel power_level(const PowerSettings &settings, int percent,
                       int millivolts, PowerLevel previous);

const char *power_level_name(PowerLevel level);

// --- Camera reads and check-ins ---

struct Settings {
    Plan camera;
    // Interval of the backend check-ins in minutes; 0: none on schedule.
    std::uint32_t checkin_min = 0;
    // Upper bound for the check-in interval after back-off and a low hub
    // battery have stretched it; 0: only kMaxStretchedMin. Telegram keeps
    // button presses and commands for about a day, so it sets 12 h. A
    // critical hub battery is not bounded by it.
    std::uint32_t checkin_max_min = 0;
    // The hub has complete uplink settings.
    bool uplink = false;
    // Camera battery below this percentage stretches the read interval by
    // low_battery_factor; 0: off.
    int low_battery_percent = 0;
    std::uint32_t low_battery_factor = 1;
    // The hub's own battery.
    PowerSettings power;
};

// Kept in RTC memory across deep sleep: only plain fields.
struct State {
    // Start of the last cycle with a camera part.
    std::int64_t last_camera;
    // Start of the last cycle with a backend sync (scan or upload).
    std::int64_t last_checkin;
    // Start of the running cycle; kNever when idle.
    std::int64_t cycle_start;
    std::uint32_t camera_failures;
    std::uint32_t checkin_failures;
    // Battery of the camera read last; -1: unknown.
    std::int32_t camera_battery;
    // The running cycle has a camera part.
    bool cycle_camera;
    // A read was asked for (by the backend) and is due now.
    bool camera_requested;
    // The hub's own battery as read last; -1: unknown.
    std::int32_t hub_battery;
    std::int32_t hub_millivolts;
    // The cell is being charged (the gauge reports a rising charge).
    bool hub_charging;
    // PowerLevel of the hub battery.
    std::uint8_t hub_level;
};

State initial_state();

struct Times {
    std::int64_t camera = kNever;
    std::int64_t checkin = kNever;
};

// Stores a reading of the hub battery and updates the level. Returns true if
// the level changed.
bool set_hub_battery(State &state, const PowerSettings &settings, int percent,
                     int millivolts, bool charging);

PowerLevel hub_level(const State &state);

// Factor for the camera read interval: back-off after failed reads and the
// stretches for a low camera battery and a low hub battery.
std::uint32_t camera_factor(const Settings &settings, const State &state);

// Why the schedule is stretched now, most important first.
enum class Stretch {
    none,
    hub_battery_critical,  // no camera reads, check-ins at a long interval
    hub_battery_low,       // camera reads and check-ins stretched
    camera_battery_low,    // camera reads stretched
};

Stretch stretch(const Settings &settings, const State &state);

// "hubBatteryCritical", "hubBatteryLow", "cameraBatteryLow"; "" for none.
const char *stretch_name(Stretch stretch);

// In the critical level of the hub battery there is no camera read, also no
// requested one (it waits until the level is left).
Times next_times(const Settings &settings, const State &state, std::int64_t now,
                 const LocalMinute &local);

enum class Task {
    none,
    camera,   // a scan: camera, then the backend sync if configured
    checkin,  // an upload: backend sync only
};

// What is due at `now`; a camera read comes first (it syncs as well).
Task due(const Times &times, std::int64_t now);

// A cycle started; `camera` if it has a camera part.
void cycle_started(State &state, bool camera, std::int64_t now);

struct CycleEnd {
    // The camera part ended with a camera session.
    bool camera_ok = false;
    // The camera part failed (all attempts); not set for a stopped cycle.
    bool camera_failed = false;
    // A backend sync was tried (the home Wi-Fi was joined or tried).
    bool sync_attempted = false;
    bool sync_ok = false;
};

void cycle_finished(State &state, const CycleEnd &end);

// The state as it will be once the running cycle's sync succeeds: what the
// hub tells the backend while it checks in.
State after_successful_sync(const State &state);

// --- Reads asked for by the backend ("readCamera" commands) ---

// Commands come with every check-in until they are acknowledged, each for one
// camera. A new one asks for a read; once a scan has been tried, the next
// check-in acknowledges the command: done if that scan read the camera it was
// for, failed otherwise (a scan reads every camera that answers, so another
// camera being read does not count). Kept in RTC memory: only plain fields.
struct ReadRequests {
    static constexpr std::size_t kMax = 4;
    static constexpr std::size_t kIdSize = 48;
    static constexpr std::size_t kCamIdSize = 40;

    enum class Reply {
        pending,  // no scan since the command arrived: do not acknowledge
        done,     // a scan since read the camera: acknowledge as done
        failed,   // a scan since did not read it: acknowledge as failed
    };

    struct Entry {
        char id[kIdSize];
        // The camera the read is for; empty: any camera.
        char cam_id[kCamIdSize];
        // 0 free, 1 pending, 2 done, 3 failed.
        std::uint8_t state;
    };
    Entry entries[kMax];

    // A command for `cam_id` arrived. `is_new` is set when it was not known:
    // a read is needed then. An answer other than pending forgets the
    // command. A camera id that does not fit is treated as "any camera".
    Reply on_command(const std::string &id, const std::string &cam_id,
                     bool *is_new);

    // The running scan read `cam_id` (fully or in part): its pending
    // commands are done.
    void on_camera_read(const std::string &cam_id);
    // The camera part of the running scan is over: pending commands whose
    // camera was not read have failed.
    void on_camera_part_ended();

    bool any_pending() const;
    void clear();
};

// --- Where a setting comes from ---

enum class Origin {
    built_in,  // Kconfig default of the profile
    local,     // stored on the hub (console, web installer)
    remote,    // set from the backend
};

const char *origin_name(Origin origin);

// The remote value if given, else the local one if given, else the built-in.
const std::string &pick(const std::string *remote, const std::string *local,
                        const std::string &built_in, Origin *origin);

}  // namespace schedule
