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

// A bound at the sun: "sunrise", "sunset", with an offset. Returns false if
// `text` is no such bound.
bool parse_sun_bound(const std::string &text, std::uint8_t &sun, int &offset)
{
    std::size_t name_len = 0;
    if (text.compare(0, 7, "sunrise") == 0) {
        sun = 1;
        name_len = 7;
    } else if (text.compare(0, 6, "sunset") == 0) {
        sun = 2;
        name_len = 6;
    } else {
        return false;
    }
    offset = 0;
    if (text.size() == name_len) {
        return true;
    }
    if ((text[name_len] != '+' && text[name_len] != '-') ||
        text.size() == name_len + 1 || text.size() > name_len + 4) {
        return false;
    }
    long value = 0;
    for (std::size_t i = name_len + 1; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    if (value > 720) {
        return false;
    }
    offset = text[name_len] == '-' ? (int)-value : (int)value;
    return true;
}

// A bound as text: "06:30", "sunrise-60", "sunset".
std::string bound_text(std::uint8_t sun, int clock, int offset)
{
    if (sun == 0) {
        char text[24];
        std::snprintf(text, sizeof(text), "%02d:%02d", clock / 60, clock % 60);
        return text;
    }
    std::string text = sun == 1 ? "sunrise" : "sunset";
    if (offset != 0) {
        text += (offset < 0 ? "-" : "+") +
                std::to_string(offset < 0 ? -offset : offset);
    }
    return text;
}

// The minutes of a bound on a day; -1 if the sun time is unknown.
int resolve_bound(std::uint8_t sun, int clock, int offset, const SunDay &day)
{
    if (sun == 0) {
        return clock;
    }
    const int base = sun == 1 ? day.sunrise : day.sunset;
    if (base < 0) {
        return -1;
    }
    return ((base + offset) % 1440 + 1440) % 1440;
}

// The window as local minutes of the day; false if a sun time is unknown or
// the window is empty on that day.
bool resolve(const Window &window, const SunDay &day, Window &out)
{
    const int start =
        resolve_bound(window.start_sun, window.start, window.start_offset, day);
    const int end =
        resolve_bound(window.end_sun, window.end, window.end_offset, day);
    if (start < 0 || end < 0 || start == end) {
        return false;
    }
    out = window;
    out.start = static_cast<std::uint16_t>(start);
    out.end = static_cast<std::uint16_t>(end);
    return true;
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
        const std::size_t dots = range.find("..");
        if (dots != std::string::npos) {
            Window window;
            const std::string first = range.substr(0, dots);
            const std::string second = range.substr(dots + 2);
            int start_clock = 0;
            int end_clock = 0;
            int offset = 0;
            if (parse_sun_bound(first, window.start_sun, offset)) {
                window.start_offset = static_cast<std::int16_t>(offset);
            } else {
                start_clock = parse_clock(first);
                if (start_clock < 0 || start_clock == 1440) {
                    return fail(error, "window '" + range +
                                           "' needs HH:MM, sunrise or sunset "
                                           "(with +-minutes) on both sides");
                }
            }
            if (parse_sun_bound(second, window.end_sun, offset)) {
                window.end_offset = static_cast<std::int16_t>(offset);
            } else {
                end_clock = parse_clock(second);
                if (end_clock < 0) {
                    return fail(error, "window '" + range +
                                           "' needs HH:MM, sunrise or sunset "
                                           "(with +-minutes) on both sides");
                }
            }
            if (window.start_sun == 0 && window.end_sun == 0 &&
                start_clock == end_clock) {
                return fail(error, "window '" + range + "' is empty");
            }
            if (window.start_sun == window.end_sun && window.start_sun != 0 &&
                window.start_offset == window.end_offset) {
                return fail(error, "window '" + range + "' is empty");
            }
            if (plan.windows.size() >= kMaxWindows) {
                return fail(error, "at most 8 windows");
            }
            window.start = static_cast<std::uint16_t>(start_clock);
            window.end = static_cast<std::uint16_t>(end_clock);
            window.interval_min = static_cast<std::uint16_t>(minutes);
            plan.windows.push_back(window);
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

bool parse_quiet(const std::string &text, Quiet &out, std::string *error)
{
    std::string trimmed;
    for (char c : text) {
        if (!is_separator(c)) {
            trimmed += c;
        }
    }
    if (trimmed.empty() || trimmed == "off") {
        out = Quiet{};
        return true;
    }
    const std::size_t dash = trimmed.find('-');
    const int start =
        dash == std::string::npos ? -1 : parse_clock(trimmed.substr(0, dash));
    const int stop =
        dash == std::string::npos ? -1 : parse_clock(trimmed.substr(dash + 1));
    if (start < 0 || stop < 0 || start == 1440) {
        return fail(error,
                    "quiet hours '" + trimmed + "' must look like HH:MM-HH:MM");
    }
    if (start == stop) {
        return fail(error, "quiet hours '" + trimmed + "' are empty");
    }
    Quiet quiet;
    quiet.start = static_cast<std::uint16_t>(start);
    quiet.end = static_cast<std::uint16_t>(stop);
    out = quiet;
    return true;
}

std::string format_quiet(const Quiet &quiet)
{
    if (!quiet.enabled()) {
        return "off";
    }
    return clock_text(quiet.start) + "-" + clock_text(quiet.end);
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
        if (window.uses_sun()) {
            text += bound_text(window.start_sun, window.start,
                               window.start_offset) +
                    ".." +
                    bound_text(window.end_sun, window.end, window.end_offset) +
                    "=" + std::to_string(window.interval_min);
        } else {
            text += clock_text(window.start) + "-" + clock_text(window.end) +
                    "=" + std::to_string(window.interval_min);
        }
    }
    if (!text.empty()) {
        text += ",";
    }
    text += "*=" + std::to_string(plan.default_min);
    return text;
}

namespace
{

// The window that applies at a minute (the first that contains it, with its
// sun bounds resolved); nullptr if none does.
const Window *window_at(const Plan &plan, int minute_of_day, const SunDay &sun,
                        Window &storage)
{
    for (const Window &window : plan.windows) {
        if (!window.uses_sun()) {
            if (contains(window, minute_of_day)) {
                return &window;
            }
            continue;
        }
        Window resolved;
        if (resolve(window, sun, resolved) &&
            contains(resolved, minute_of_day)) {
            storage = resolved;
            return &storage;
        }
    }
    return nullptr;
}

}  // namespace

std::uint32_t interval_at(const Plan &plan, int minute_of_day,
                          const SunDay &sun)
{
    Window storage;
    const Window *window = window_at(plan, minute_of_day, sun, storage);
    return window != nullptr ? window->interval_min : plan.default_min;
}

bool in_sun_window(const Plan &plan, int minute_of_day, const SunDay &sun)
{
    Window storage;
    const Window *window = window_at(plan, minute_of_day, sun, storage);
    return window != nullptr && window->uses_sun() && window->interval_min > 0;
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
                      std::uint32_t factor, const LocalMinute &local,
                      const SunMinutes &sun)
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
        const std::uint32_t interval =
            interval_at(plan, local(t), sun ? sun(t) : SunDay());
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

bool in_quiet(const Quiet &quiet, std::int64_t from, const LocalMinute &local,
              const Plan *plan, const SunMinutes &sun)
{
    if (!quiet.enabled() || !local) {
        return false;
    }
    Window window;
    window.start = quiet.start;
    window.end = quiet.end;
    const int minute = local(from);
    if (!contains(window, minute)) {
        return false;
    }
    // A read window at the sun wins over the quiet hours.
    return !(plan != nullptr && sun && in_sun_window(*plan, minute, sun(from)));
}

std::int64_t quiet_end(const Quiet &quiet, std::int64_t from,
                       const LocalMinute &local, const Plan *plan,
                       const SunMinutes &sun)
{
    if (!in_quiet(quiet, from, local, plan, sun)) {
        return from;
    }
    // Minute by minute, as in next_due(): finds the end also across a change
    // of the local time (summer time). A window is shorter than a day.
    std::int64_t t = from;
    while (t <= from + 25 * 3600) {
        if (!in_quiet(quiet, t, local, plan, sun)) {
            return t;
        }
        t += 60;
    }
    return from;
}

Times next_times(const Settings &settings, const State &state, std::int64_t now,
                 const LocalMinute &local, const SunMinutes &sun)
{
    Times times;
    const bool critical = hub_level(state) == PowerLevel::critical;
    if (critical) {
        times.camera = kNever;
    } else if (state.camera_requested) {
        times.camera = now;
    } else {
        times.camera = next_due(settings.camera, state.last_camera, now,
                                camera_factor(settings, state), local, sun);
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
            std::uint32_t base = settings.checkin_min;
            if (settings.checkin_energy_permille < 1000 &&
                !hub_stretch_low(settings, state)) {
                const std::uint32_t reduced = static_cast<std::uint32_t>(
                    (std::uint64_t)base * settings.checkin_energy_permille /
                    1000);
                const std::uint32_t floor_min = settings.min_interval_min < base
                                                    ? settings.min_interval_min
                                                    : base;
                base = reduced > floor_min ? reduced : floor_min;
            }
            interval_min = stretched(base, factor);
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
    // Quiet hours: what falls into them waits for their end, also a read the
    // app asked for.
    if (settings.quiet.enabled()) {
        if (times.camera != kNever) {
            times.camera = quiet_end(settings.quiet, times.camera, local,
                                     &settings.camera, sun);
        }
        if (times.checkin != kNever) {
            times.checkin = quiet_end(settings.quiet, times.checkin, local,
                                      &settings.camera, sun);
        }
    }
    // A read due shortly after the check-in runs with it: the read syncs
    // anyway, so the hub wakes once instead of twice (#436).
    if (settings.align_min > 0 && times.camera != kNever &&
        times.checkin != kNever && times.camera > times.checkin &&
        times.camera - times.checkin <= (std::int64_t)settings.align_min * 60) {
        times.camera = times.checkin;
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
    case Origin::gnss:
        return "gnss";
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
