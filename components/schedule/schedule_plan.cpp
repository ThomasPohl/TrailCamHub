#include "schedule_plan.hpp"

#include <cstdio>
#include <cstring>

namespace schedule
{

namespace
{

bool is_separator(char c)
{
    return c == ',' || c == ' ' || c == '\t' || c == ';';
}

// Reads "HH:MM" (00:00-24:00) at `text`; returns minutes or -1.
int parse_clock(const std::string &text)
{
    if (text.size() != 5 || text[2] != ':') {
        return -1;
    }
    for (std::size_t i : {0u, 1u, 3u, 4u}) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
    }
    const int hours = (text[0] - '0') * 10 + (text[1] - '0');
    const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    if (minutes > 59 || hours > 24 || (hours == 24 && minutes != 0)) {
        return -1;
    }
    return hours * 60 + minutes;
}

// Reads a whole number of minutes (0-kMaxIntervalMin); -1 if invalid.
long parse_minutes(const std::string &text)
{
    if (text.empty() || text.size() > 5) {
        return -1;
    }
    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value <= (long)kMaxIntervalMin ? value : -1;
}

bool fail(std::string *error, const std::string &reason)
{
    if (error != nullptr) {
        *error = reason;
    }
    return false;
}

bool contains(const Window &window, int minute)
{
    if (window.start < window.end) {
        return minute >= window.start && minute < window.end;
    }
    return minute >= window.start || minute < window.end;
}

std::string clock_text(int minutes)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
    return text;
}

}  // namespace

bool Plan::reads() const
{
    if (default_min > 0) {
        return true;
    }
    for (const Window &window : windows) {
        if (window.interval_min > 0) {
            return true;
        }
    }
    return false;
}

bool parse_plan(const std::string &text, Plan &out, std::string *error)
{
    Plan plan;
    bool have_default = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (is_separator(text[pos])) {
            pos++;
            continue;
        }
        std::size_t end = pos;
        while (end < text.size() && !is_separator(text[end])) {
            end++;
        }
        const std::string entry = text.substr(pos, end - pos);
        pos = end;

        if (entry == "off") {
            continue;
        }
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) {
            return fail(error, "entry '" + entry + "' has no '=MIN'");
        }
        const std::string range = entry.substr(0, equals);
        const long minutes = parse_minutes(entry.substr(equals + 1));
        if (minutes < 0) {
            return fail(error,
                        "interval in '" + entry + "' must be 0-1440 minutes");
        }
        if (range == "*") {
            if (have_default) {
                return fail(error, "'*' is given twice");
            }
            have_default = true;
            plan.default_min = static_cast<std::uint16_t>(minutes);
            continue;
        }
        const std::size_t dash = range.find('-');
        const int start =
            dash == std::string::npos ? -1 : parse_clock(range.substr(0, dash));
        const int stop = dash == std::string::npos
                             ? -1
                             : parse_clock(range.substr(dash + 1));
        if (start < 0 || stop < 0 || start == 1440) {
            return fail(error,
                        "window '" + range + "' must look like HH:MM-HH:MM");
        }
        if (start == stop) {
            return fail(error, "window '" + range + "' is empty");
        }
        if (plan.windows.size() >= kMaxWindows) {
            return fail(error, "at most 8 windows");
        }
        Window window;
        window.start = static_cast<std::uint16_t>(start);
        window.end = static_cast<std::uint16_t>(stop);
        window.interval_min = static_cast<std::uint16_t>(minutes);
        plan.windows.push_back(window);
    }
    out = plan;
    return true;
}

std::string format_plan(const Plan &plan)
{
    if (!plan.reads()) {
        return "off";
    }
    std::string text;
    for (const Window &window : plan.windows) {
        if (!text.empty()) {
            text += ",";
        }
        text += clock_text(window.start) + "-" + clock_text(window.end) + "=" +
                std::to_string(window.interval_min);
    }
    if (!text.empty()) {
        text += ",";
    }
    text += "*=" + std::to_string(plan.default_min);
    return text;
}

std::uint32_t interval_at(const Plan &plan, int minute_of_day)
{
    for (const Window &window : plan.windows) {
        if (contains(window, minute_of_day)) {
            return window.interval_min;
        }
    }
    return plan.default_min;
}

std::uint32_t clockless_interval(const Plan &plan)
{
    if (plan.default_min > 0) {
        return plan.default_min;
    }
    std::uint32_t longest = 0;
    for (const Window &window : plan.windows) {
        if (window.interval_min > longest) {
            longest = window.interval_min;
        }
    }
    return longest;
}

std::uint32_t backoff_factor(std::uint32_t failures)
{
    return 1u << (failures < 4 ? failures : 4);
}

std::uint32_t stretched(std::uint32_t interval_min, std::uint32_t factor)
{
    if (factor <= 1) {
        return interval_min;
    }
    const std::uint32_t limit =
        interval_min > kMaxStretchedMin ? interval_min : kMaxStretchedMin;
    const std::uint64_t value = (std::uint64_t)interval_min * factor;
    return value > limit ? limit : static_cast<std::uint32_t>(value);
}

std::int64_t next_due(const Plan &plan, std::int64_t last, std::int64_t now,
                      std::uint32_t factor, const LocalMinute &local)
{
    if (!plan.reads()) {
        return kNever;
    }
    if (!local) {
        const std::uint32_t interval = clockless_interval(plan);
        if (last == kNever) {
            return now;
        }
        const std::int64_t due =
            last + (std::int64_t)stretched(interval, factor) * 60;
        return due > now ? due : now;
    }
    // Walk forward in steps of a minute (and exactly to the moment the
    // interval is reached), so that window starts, the end of an interval
    // and changes of the local time (summer time) are all found.
    std::int64_t t = now;
    while (t <= now + kHorizonS) {
        const std::uint32_t interval = interval_at(plan, local(t));
        std::int64_t step = 60;
        if (interval > 0) {
            const std::int64_t need =
                (std::int64_t)stretched(interval, factor) * 60;
            if (last == kNever || t - last >= need) {
                return t;
            }
            if (last + need - t < step) {
                step = last + need - t;
            }
        }
        t += step;
    }
    return kNever;
}

State initial_state()
{
    State state;
    std::memset(&state, 0, sizeof(state));
    state.last_camera = kNever;
    state.last_checkin = kNever;
    state.cycle_start = kNever;
    state.camera_battery = -1;
    state.hub_battery = -1;
    state.hub_millivolts = -1;
    state.hub_level = (std::uint8_t)PowerLevel::normal;
    return state;
}

namespace
{

// `value` is below `threshold` (both known and the threshold on).
bool below(int value, int threshold)
{
    return threshold > 0 && value >= 0 && value < threshold;
}

// The battery is clearly back above `threshold` (or the value is unknown,
// or the threshold off).
bool recovered(int value, int threshold, int margin)
{
    return threshold <= 0 || value < 0 || value >= threshold + margin;
}

bool camera_battery_low(const Settings &settings, const State &state)
{
    return settings.low_battery_percent > 0 && state.camera_battery >= 0 &&
           state.camera_battery < settings.low_battery_percent &&
           settings.low_battery_factor > 1;
}

// The low level applies to the intervals: not while the cell charges.
bool hub_stretch_low(const Settings &settings, const State &state)
{
    return hub_level(state) == PowerLevel::low && !state.hub_charging &&
           (settings.power.low_camera_factor > 1 ||
            settings.power.low_checkin_factor > 1);
}

}  // namespace

PowerLevel power_level(const PowerSettings &settings, int percent,
                       int millivolts, PowerLevel previous)
{
    if (percent < 0 && millivolts < 0) {
        return PowerLevel::normal;
    }
    if (below(percent, settings.critical_percent) ||
        below(millivolts, settings.critical_mv)) {
        return PowerLevel::critical;
    }
    if (previous == PowerLevel::critical &&
        !(recovered(percent, settings.critical_percent,
                    settings.hysteresis_percent) &&
          recovered(millivolts, settings.critical_mv,
                    settings.hysteresis_mv))) {
        return PowerLevel::critical;
    }
    if (below(percent, settings.low_percent)) {
        return PowerLevel::low;
    }
    if (previous != PowerLevel::normal &&
        !recovered(percent, settings.low_percent,
                   settings.hysteresis_percent)) {
        return PowerLevel::low;
    }
    return PowerLevel::normal;
}

const char *power_level_name(PowerLevel level)
{
    switch (level) {
    case PowerLevel::normal:
        return "normal";
    case PowerLevel::low:
        return "low";
    case PowerLevel::critical:
        return "critical";
    }
    return "normal";
}

bool set_hub_battery(State &state, const PowerSettings &settings, int percent,
                     int millivolts, bool charging)
{
    const PowerLevel before = hub_level(state);
    state.hub_battery = percent;
    state.hub_millivolts = millivolts;
    state.hub_charging = charging;
    const PowerLevel after = power_level(settings, percent, millivolts, before);
    state.hub_level = (std::uint8_t)after;
    return after != before;
}

PowerLevel hub_level(const State &state)
{
    return state.hub_level <= (std::uint8_t)PowerLevel::critical
               ? (PowerLevel)state.hub_level
               : PowerLevel::normal;
}

std::uint32_t camera_factor(const Settings &settings, const State &state)
{
    std::uint32_t factor = backoff_factor(state.camera_failures);
    if (camera_battery_low(settings, state)) {
        factor *= settings.low_battery_factor;
    }
    if (hub_stretch_low(settings, state) &&
        settings.power.low_camera_factor > 1) {
        factor *= settings.power.low_camera_factor;
    }
    return factor;
}

Stretch stretch(const Settings &settings, const State &state)
{
    if (hub_level(state) == PowerLevel::critical) {
        return Stretch::hub_battery_critical;
    }
    if (hub_stretch_low(settings, state)) {
        return Stretch::hub_battery_low;
    }
    if (camera_battery_low(settings, state)) {
        return Stretch::camera_battery_low;
    }
    return Stretch::none;
}

const char *stretch_name(Stretch stretch)
{
    switch (stretch) {
    case Stretch::none:
        return "";
    case Stretch::hub_battery_critical:
        return "hubBatteryCritical";
    case Stretch::hub_battery_low:
        return "hubBatteryLow";
    case Stretch::camera_battery_low:
        return "cameraBatteryLow";
    }
    return "";
}

Times next_times(const Settings &settings, const State &state, std::int64_t now,
                 const LocalMinute &local)
{
    Times times;
    const bool critical = hub_level(state) == PowerLevel::critical;
    if (critical) {
        times.camera = kNever;
    } else if (state.camera_requested) {
        times.camera = now;
    } else {
        times.camera = next_due(settings.camera, state.last_camera, now,
                                camera_factor(settings, state), local);
    }
    if (settings.uplink && settings.checkin_min > 0) {
        std::uint32_t interval_min;
        if (critical) {
            // A fixed, long interval: no back-off on top, so the hub keeps
            // reporting its battery.
            interval_min =
                settings.power.critical_checkin_min > settings.checkin_min
                    ? settings.power.critical_checkin_min
                    : settings.checkin_min;
        } else {
            std::uint32_t factor = backoff_factor(state.checkin_failures);
            if (hub_stretch_low(settings, state) &&
                settings.power.low_checkin_factor > 1) {
                factor *= settings.power.low_checkin_factor;
            }
            interval_min = stretched(settings.checkin_min, factor);
            if (settings.checkin_max_min > 0 &&
                interval_min > settings.checkin_max_min) {
                interval_min = settings.checkin_max_min > settings.checkin_min
                                   ? settings.checkin_max_min
                                   : settings.checkin_min;
            }
        }
        if (state.last_checkin == kNever) {
            times.checkin = now;
        } else {
            const std::int64_t due =
                state.last_checkin + (std::int64_t)interval_min * 60;
            times.checkin = due > now ? due : now;
        }
    }
    return times;
}

Task due(const Times &times, std::int64_t now)
{
    if (times.camera != kNever && times.camera <= now) {
        return Task::camera;
    }
    if (times.checkin != kNever && times.checkin <= now) {
        return Task::checkin;
    }
    return Task::none;
}

void cycle_started(State &state, bool camera, std::int64_t now)
{
    state.cycle_start = now;
    state.cycle_camera = camera;
    if (camera) {
        state.last_camera = now;
        state.camera_requested = false;
    }
}

void cycle_finished(State &state, const CycleEnd &end)
{
    if (state.cycle_camera) {
        if (end.camera_ok) {
            state.camera_failures = 0;
        } else if (end.camera_failed) {
            state.camera_failures++;
        }
    }
    if (end.sync_attempted && state.cycle_start != kNever) {
        state.last_checkin = state.cycle_start;
        state.checkin_failures = end.sync_ok ? 0 : state.checkin_failures + 1;
    }
    state.cycle_start = kNever;
    state.cycle_camera = false;
}

State after_successful_sync(const State &state)
{
    State next = state;
    CycleEnd end;
    end.camera_ok = state.cycle_camera;
    end.sync_attempted = true;
    end.sync_ok = true;
    cycle_finished(next, end);
    return next;
}

ReadRequests::Reply ReadRequests::on_command(const std::string &id,
                                             const std::string &cam_id,
                                             bool *is_new)
{
    if (is_new != nullptr) {
        *is_new = false;
    }
    for (Entry &entry : entries) {
        if (entry.state != 0 && id == entry.id) {
            const std::uint8_t state = entry.state;
            if (state == 1) {
                return Reply::pending;
            }
            entry.state = 0;
            return state == 2 ? Reply::done : Reply::failed;
        }
    }
    if (is_new != nullptr) {
        *is_new = true;
    }
    if (id.size() >= kIdSize) {
        // Cannot be tracked; it asks for a read every time it comes.
        return Reply::pending;
    }
    // A free slot, else the oldest answered one; with all slots pending the
    // read asked for already covers this command at its next arrival.
    for (std::uint8_t wanted : {0, 2, 3}) {
        for (Entry &entry : entries) {
            if (entry.state == wanted) {
                std::memcpy(entry.id, id.c_str(), id.size() + 1);
                entry.cam_id[0] = '\0';
                if (cam_id.size() < kCamIdSize) {
                    std::memcpy(entry.cam_id, cam_id.c_str(),
                                cam_id.size() + 1);
                }
                entry.state = 1;
                return Reply::pending;
            }
        }
    }
    return Reply::pending;
}

void ReadRequests::on_camera_read(const std::string &cam_id)
{
    for (Entry &entry : entries) {
        if (entry.state == 1 &&
            (entry.cam_id[0] == '\0' || cam_id == entry.cam_id)) {
            entry.state = 2;
        }
    }
}

void ReadRequests::on_camera_part_ended()
{
    for (Entry &entry : entries) {
        if (entry.state == 1) {
            entry.state = 3;
        }
    }
}

bool ReadRequests::any_pending() const
{
    for (const Entry &entry : entries) {
        if (entry.state == 1) {
            return true;
        }
    }
    return false;
}

void ReadRequests::clear()
{
    std::memset(entries, 0, sizeof(entries));
}

const char *origin_name(Origin origin)
{
    switch (origin) {
    case Origin::built_in:
        return "built-in";
    case Origin::local:
        return "local";
    case Origin::remote:
        return "remote";
    }
    return "built-in";
}

const std::string &pick(const std::string *remote, const std::string *local,
                        const std::string &built_in, Origin *origin)
{
    if (remote != nullptr && !remote->empty()) {
        if (origin != nullptr) {
            *origin = Origin::remote;
        }
        return *remote;
    }
    if (local != nullptr && !local->empty()) {
        if (origin != nullptr) {
            *origin = Origin::local;
        }
        return *local;
    }
    if (origin != nullptr) {
        *origin = Origin::built_in;
    }
    return built_in;
}

}  // namespace schedule
