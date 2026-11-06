/*
 * Keeps the schedule state in RTC memory, which survives deep sleep (but not
 * a power-on or reset), measures schedule time across deep sleep and stores
 * the settings that the backend sends in their own NVS namespace.
 */

#include "schedule_state.hpp"

#include <atomic>
#include <cstring>
#include <sys/time.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "hub_config.hpp"
#include "time_sync.h"

#ifndef CONFIG_SCHEDULE_CAMERA_PLAN
#error "main/Kconfig.projbuild was not processed"
#endif

namespace schedule
{

namespace
{

const char *TAG = "trailcamhub_schedule";
const char *kNamespace = "schedule";
const char *kRemotePlanKey = "plan";
const char *kRemoteCheckinKey = "checkin_min";
// Changed with the layout of RtcData: "TCH2" since read requests carry a
// camera id.
constexpr std::uint32_t kMagic = 0x54434832;

// Everything that has to survive deep sleep.
struct RtcData {
    std::uint32_t magic;
    // Schedule time and wall clock (microseconds) when the hub went to sleep.
    std::int64_t mono_at_sleep_us;
    std::int64_t wall_at_sleep_us;
    std::int64_t planned_sleep_us;
    State state;
    ReadRequests reads;
};

RTC_DATA_ATTR RtcData s_rtc;

SemaphoreHandle_t s_lock;
bool s_woke;
// Schedule microseconds = offset + esp_timer_get_time().
std::int64_t s_mono_offset_us;
std::atomic<std::int64_t> s_last_activity{kNever};
void (*s_change_callback)(void);

// Remote settings as stored ("" = not set); guarded by s_lock.
std::string s_remote_plan;
std::string s_remote_checkin;

class Lock
{
  public:
    Lock()
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    ~Lock()
    {
        xSemaphoreGive(s_lock);
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

std::int64_t wall_us()
{
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return (std::int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

void changed()
{
    void (*callback)(void) = s_change_callback;
    if (callback != nullptr) {
        callback();
    }
}

std::string read_nvs_string(nvs_handle_t handle, const char *key)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0 ||
        length > 256) {
        return "";
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, &value[0], &length) != ESP_OK) {
        return "";
    }
    value.resize(length - 1);
    return value;
}

void load_remote()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;  // nothing stored yet
    }
    s_remote_plan = read_nvs_string(handle, kRemotePlanKey);
    s_remote_checkin = read_nvs_string(handle, kRemoteCheckinKey);
    nvs_close(handle);
}

// Stores or erases (empty value) one remote setting.
bool store_remote(const char *key, const std::string &value)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open %s: %s", kNamespace,
                 esp_err_to_name(error));
        return false;
    }
    error = value.empty() ? nvs_erase_key(handle, key)
                          : nvs_set_str(handle, key, value.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not store %s: %s", key, esp_err_to_name(error));
    }
    return error == ESP_OK;
}

// Local minute of the day of a schedule time, through the wall clock.
int local_minute_at(std::int64_t t, std::int64_t now_mono,
                    std::int64_t now_wall)
{
    const time_t wall = static_cast<time_t>(now_wall + (t - now_mono));
    struct tm local = {};
    localtime_r(&wall, &local);
    return local.tm_hour * 60 + local.tm_min;
}

Times times_for(const Effective &effective, const State &state)
{
    const std::int64_t mono = now();
    if (!time_sync_clock_valid()) {
        return next_times(effective.settings, state, mono, LocalMinute());
    }
    const std::int64_t wall = time(nullptr);
    const LocalMinute local = [mono, wall](std::int64_t t) {
        return local_minute_at(t, mono, wall);
    };
    return next_times(effective.settings, state, mono, local);
}

std::uint32_t minutes_of(const std::string &text, std::uint32_t fallback)
{
    if (text.empty() || text.size() > 4) {
        return fallback;
    }
    std::uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return fallback;
        }
        value = value * 10 + (std::uint32_t)(c - '0');
    }
    return value <= kMaxIntervalMin ? value : fallback;
}

}  // namespace

void init()
{
    s_lock = xSemaphoreCreateMutex();
    s_woke = esp_reset_reason() == ESP_RST_DEEPSLEEP && s_rtc.magic == kMagic;
    if (s_woke) {
        // The wall clock keeps running in deep sleep; the time it moved,
        // less the time since this boot, is the time asleep (including the
        // boot). A clock set while asleep is impossible, but a value that
        // makes no sense falls back to the planned sleep.
        const std::int64_t slept =
            wall_us() - esp_timer_get_time() - s_rtc.wall_at_sleep_us;
        const bool plausible =
            slept >= 0 &&
            slept <= 2 * s_rtc.planned_sleep_us + 3600LL * 1000000;
        s_mono_offset_us = s_rtc.mono_at_sleep_us +
                           (plausible ? slept : s_rtc.planned_sleep_us);
        // No cycle runs across sleep.
        s_rtc.state.cycle_start = kNever;
        s_rtc.state.cycle_camera = false;
        ESP_LOGI(TAG, "Woke up after %lld s of sleep",
                 (long long)((plausible ? slept : s_rtc.planned_sleep_us) /
                             1000000));
    } else {
        std::memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = kMagic;
        s_rtc.state = initial_state();
        s_rtc.reads.clear();
        s_mono_offset_us = 0;
    }
    load_remote();
    if (!s_remote_plan.empty() || !s_remote_checkin.empty()) {
        ESP_LOGI(TAG, "Schedule from the backend: plan \"%s\", check-in %s min",
                 s_remote_plan.c_str(),
                 s_remote_checkin.empty() ? "(default)"
                                          : s_remote_checkin.c_str());
    }
}

bool woke_from_sleep()
{
    return s_woke;
}

std::int64_t now()
{
    return (s_mono_offset_us + esp_timer_get_time()) / 1000000;
}

std::int64_t to_unix(std::int64_t t)
{
    if (t == kNever || !time_sync_clock_valid()) {
        return 0;
    }
    return time(nullptr) + (t - now());
}

Effective effective()
{
    hub_config::Settings local;
    if (hub_config::load(local) != ESP_OK) {
        local = hub_config::Settings{};
    }
    std::string remote_plan;
    std::string remote_checkin;
    {
        Lock lock;
        remote_plan = s_remote_plan;
        remote_checkin = s_remote_checkin;
    }

    Effective out;
    static const std::string kBuiltInPlan = CONFIG_SCHEDULE_CAMERA_PLAN;
    static const std::string kBuiltInCheckin =
        std::to_string(CONFIG_SCHEDULE_CHECKIN_INTERVAL_MIN);
    out.camera_plan = pick(&remote_plan, &local.camera_plan, kBuiltInPlan,
                           &out.camera_plan_origin);
    std::string error;
    if (!parse_plan(out.camera_plan, out.settings.camera, &error)) {
        // Stored values are validated; only a bad Kconfig value gets here.
        ESP_LOGW(TAG, "Camera plan \"%s\" ignored: %s", out.camera_plan.c_str(),
                 error.c_str());
        out.settings.camera = Plan{};
    }
    out.camera_plan = format_plan(out.settings.camera);
    out.checkin_min = minutes_of(pick(&remote_checkin, &local.checkin_min,
                                      kBuiltInCheckin, &out.checkin_origin),
                                 CONFIG_SCHEDULE_CHECKIN_INTERVAL_MIN);
    out.settings.checkin_min = out.checkin_min;
    out.settings.uplink = hub_config::uplink_ready(local);
    out.settings.low_battery_percent = CONFIG_SCHEDULE_LOW_BATTERY_PERCENT;
    out.settings.low_battery_factor = CONFIG_SCHEDULE_LOW_BATTERY_FACTOR;
    return out;
}

bool apply_remote(const Remote &remote)
{
    // An invalid value leaves the stored one as it is.
    bool plan_valid = true;
    std::string plan;
    if (remote.has_camera_plan) {
        Plan parsed;
        std::string error = "too long";
        if (remote.camera_plan.size() <= 200 &&
            parse_plan(remote.camera_plan, parsed, &error)) {
            plan = format_plan(parsed);
        } else {
            ESP_LOGW(TAG, "Ignoring the camera plan from the backend: %s",
                     error.c_str());
            plan_valid = false;
        }
    }
    bool checkin_valid = true;
    std::string checkin;
    if (remote.checkin_interval_s > 0) {
        std::int64_t minutes = (remote.checkin_interval_s + 59) / 60;
        if (minutes > (std::int64_t)kMaxIntervalMin) {
            minutes = kMaxIntervalMin;
        }
        checkin = std::to_string(minutes);
    } else if (remote.checkin_interval_s == 0) {
        // Without scheduled check-ins the backend could not undo it.
        ESP_LOGW(TAG, "Ignoring a check-in interval of 0 from the backend");
        checkin_valid = false;
    }

    bool any_change = false;
    std::string stored_plan;
    std::string stored_checkin;
    {
        Lock lock;
        if (plan_valid && plan != s_remote_plan &&
            store_remote(kRemotePlanKey, plan)) {
            s_remote_plan = plan;
            any_change = true;
        }
        if (checkin_valid && checkin != s_remote_checkin &&
            store_remote(kRemoteCheckinKey, checkin)) {
            s_remote_checkin = checkin;
            any_change = true;
        }
        stored_plan = s_remote_plan;
        stored_checkin = s_remote_checkin;
    }
    if (any_change) {
        ESP_LOGI(TAG, "Schedule from the backend now: plan %s, check-in %s min",
                 stored_plan.empty() ? "(not set)" : stored_plan.c_str(),
                 stored_checkin.empty() ? "(not set)" : stored_checkin.c_str());
        changed();
    }
    return plan_valid && checkin_valid;
}

State state()
{
    Lock lock;
    return s_rtc.state;
}

Times next_times()
{
    return next_times(effective());
}

Times next_times(const Effective &effective)
{
    return times_for(effective, state());
}

Times next_times_after_sync()
{
    return times_for(effective(), after_successful_sync(state()));
}

void on_cycle_started(bool camera)
{
    Lock lock;
    cycle_started(s_rtc.state, camera, now());
}

void on_camera_read(const std::string &cam_id)
{
    Lock lock;
    s_rtc.reads.on_camera_read(cam_id);
}

void on_camera_part_ended()
{
    Lock lock;
    s_rtc.reads.on_camera_part_ended();
}

void on_cycle_finished(const CycleEnd &end)
{
    {
        Lock lock;
        cycle_finished(s_rtc.state, end);
    }
    changed();
}

void set_camera_battery(int percent)
{
    Lock lock;
    s_rtc.state.camera_battery = percent;
}

ReadRequests::Reply on_read_command(const std::string &id,
                                    const std::string &cam_id)
{
    bool is_new = false;
    ReadRequests::Reply reply;
    {
        Lock lock;
        reply = s_rtc.reads.on_command(id, cam_id, &is_new);
        if (is_new) {
            s_rtc.state.camera_requested = true;
        }
    }
    if (is_new) {
        ESP_LOGI(TAG, "The backend asks for a read of camera %s (%s)",
                 cam_id.c_str(), id.c_str());
        changed();
    }
    return reply;
}

void request_camera_read(const char *reason)
{
    {
        Lock lock;
        s_rtc.state.camera_requested = true;
    }
    ESP_LOGI(TAG, "A camera read is due: %s", reason);
    changed();
}

void set_change_callback(void (*callback)(void))
{
    s_change_callback = callback;
}

void note_activity()
{
    s_last_activity = now();
    changed();
}

std::int64_t last_activity()
{
    return s_last_activity.load();
}

void prepare_sleep(std::int64_t sleep_s)
{
    Lock lock;
    s_rtc.mono_at_sleep_us = s_mono_offset_us + esp_timer_get_time();
    s_rtc.wall_at_sleep_us = wall_us();
    s_rtc.planned_sleep_us = sleep_s * 1000000;
    s_rtc.magic = kMagic;
}

}  // namespace schedule
