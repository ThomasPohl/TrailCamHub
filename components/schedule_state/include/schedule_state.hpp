#pragma once

// The hub's wake-up schedule at run time: the state kept across deep sleep,
// the clock it is measured with, the settings that apply and where they come
// from, and the reads the backend asked for. The scheduler task, the backend
// uplink and the serial console share it; all functions may be called from
// any task.
//
// Schedule times are seconds on a monotonic clock that keeps running across
// deep sleep and does not jump when the wall clock is set (SNTP, backend), so
// intervals stay right while the clock is corrected. The wall clock is only
// needed for the read windows and for the times reported to the backend.

#include <cstdint>
#include <string>

#include "schedule_plan.hpp"

namespace schedule
{

// Restores the state after a wake-up from deep sleep, or starts afresh after
// a power-on or reset. Call once in app_main after nvs_flash_init().
void init();

// True if this boot is a wake-up from the hub's own deep sleep.
bool woke_from_sleep();

// Schedule seconds now.
std::int64_t now();

// Unix time of a schedule time, or 0 if the clock does not hold the time (or
// `t` is kNever).
std::int64_t to_unix(std::int64_t t);

struct Effective {
    Settings settings;
    // The camera plan as text and the check-in interval, and their origin.
    std::string camera_plan;
    Origin camera_plan_origin = Origin::built_in;
    std::uint32_t checkin_min = 0;
    Origin checkin_origin = Origin::built_in;
};

// The settings that apply now: set from the backend, else stored on the hub,
// else the defaults of the build (profile). Reads NVS.
Effective effective();

// Settings from the backend's check-in response. A missing value removes the
// remote setting, so the local or built-in one applies again.
struct Remote {
    bool has_camera_plan = false;
    std::string camera_plan;
    // Seconds; -1: not given.
    std::int64_t checkin_interval_s = -1;
};

// Stores the remote settings if they changed. Returns false if a value was
// invalid (it is then ignored).
bool apply_remote(const Remote &remote);

State state();

// Next camera read and check-in from now with the effective settings.
Times next_times();
Times next_times(const Effective &effective);

// The same as they will be once the running cycle's sync has succeeded; what
// the hub reports while it checks in.
Times next_times_after_sync();

// --- Bookkeeping (scheduler task and session status) ---

void on_cycle_started(bool camera);
// The running cycle read the camera `cam_id` (the sanitized id used on the SD
// card and in the backend). Answers its pending read requests.
void on_camera_read(const std::string &cam_id);
// The camera part of the running cycle ended: the read requests of cameras it
// did not read have failed.
void on_camera_part_ended();
void on_cycle_finished(const CycleEnd &end);
void set_camera_battery(int percent);

// A "readCamera" command in a check-in. A new one makes a read due at once.
ReadRequests::Reply on_read_command(const std::string &id,
                                    const std::string &cam_id);

// Something new needs the camera (a requested video): a read is due at once.
// Unlike on_read_command() there is nothing to acknowledge.
void request_camera_read(const char *reason);

// Called whenever something changes that may move the next cycle (settings,
// a requested read), e.g. to wake the scheduler task. Optional.
void set_change_callback(void (*callback)(void));

// A person uses the hub (serial console): it stays awake for a while.
void note_activity();
// Schedule time of the last activity; kNever if none since power-on.
std::int64_t last_activity();

// Saves what is needed to continue after deep sleep for `sleep_s` seconds.
void prepare_sleep(std::int64_t sleep_s);

}  // namespace schedule
