/*
 * The scheduler task wakes up when the session status changes, when the
 * schedule changes (settings, a read asked for by the backend, a console
 * command) and at least every half minute. When the controller is idle it
 * requests what is due, or, with deep sleep enabled, sleeps until the next
 * cycle once no one has used the hub for a while.
 */

#include "scheduler.h"

#include <atomic>
#include <cstdint>
#include <time.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "camera_protocol.h"
#include "schedule_state.hpp"
#include "sd_log.h"
#include "session_controller.h"

#define SCHEDULER_TASK_STACK 6144
#define SCHEDULER_TASK_PRIORITY 3
// Longest wait between two looks at the schedule (ms).
#define SCHEDULER_MAX_WAIT_MS 30000
// How long a request may take to show in the status (ms).
#define SCHEDULER_REQUEST_WAIT_MS 5000

namespace
{

const char *TAG = "trailcamhub_sched";

TaskHandle_t s_task;
// Only touched by the session controller task (status listener).
session_phase_t s_last_phase = SESSION_PHASE_IDLE;
bool s_session_reached;
std::atomic<bool> s_battery_pending;

void wake_task()
{
    TaskHandle_t task = s_task;
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

bool camera_phase(session_phase_t phase)
{
    return phase == SESSION_PHASE_CAMERA_WAKE ||
           phase == SESSION_PHASE_CAMERA_WIFI_JOIN ||
           phase == SESSION_PHASE_CAMERA_SESSION ||
           phase == SESSION_PHASE_CAMERA_RETRY_WAIT;
}

// Records every cycle, whoever asked for it (schedule, console, button), so
// that the next one is planned from it.
void on_status(const session_status_t *status, void *context)
{
    (void)context;
    // A listing fetches nothing: it is no read of the camera, so it neither
    // moves the next read nor answers a read that the backend asked for.
    if (status->list_only) {
        s_last_phase = status->phase;
        wake_task();
        return;
    }
    if (s_last_phase == SESSION_PHASE_IDLE &&
        status->phase != SESSION_PHASE_IDLE) {
        schedule::on_cycle_started(camera_phase(status->phase));
        s_session_reached = false;
    }
    if (status->phase == SESSION_PHASE_CAMERA_SESSION) {
        s_session_reached = true;
    }
    const bool stopped =
        status->cycle_finished && status->last_result == SESSION_RESULT_STOPPED;
    // The camera part ends with the start of the sync or with the cycle. A
    // session that ended with an error before storing anything, or found no
    // SD card in the camera, is not a read (camera_ok is false).
    if (camera_phase(s_last_phase) && !camera_phase(status->phase) &&
        !stopped) {
        schedule::on_camera_result(s_session_reached && status->camera_ok);
        if (s_session_reached) {
            s_battery_pending = true;
        }
    }
    if (status->cycle_finished) {
        schedule::CycleEnd end;
        end.camera_ok = s_session_reached && status->camera_ok && !stopped;
        // Backs off the next reads; a camera without an SD card would only
        // spend its battery on them.
        end.camera_failed =
            status->last_result == SESSION_RESULT_CAMERA_UNREACHABLE ||
            status->last_result == SESSION_RESULT_CAMERA_NO_CARD;
        end.sync_attempted =
            status->last_result == SESSION_RESULT_UPLINK_DONE ||
            status->last_result == SESSION_RESULT_UPLINK_UNREACHABLE;
        end.sync_ok = status->last_result == SESSION_RESULT_UPLINK_DONE &&
                      status->sync_ok;
        schedule::on_cycle_finished(end);
    }
    s_last_phase = status->phase;
    wake_task();
}

// "in 1234 s (15:20)" or "never".
void describe(std::int64_t t, std::int64_t now, char *text, size_t size)
{
    if (t == schedule::kNever) {
        snprintf(text, size, "never");
        return;
    }
    const std::int64_t unix_time = schedule::to_unix(t);
    if (unix_time > 0) {
        const time_t wall = static_cast<time_t>(unix_time);
        struct tm local = {};
        localtime_r(&wall, &local);
        snprintf(text, size, "in %lld s (%02d:%02d)", (long long)(t - now),
                 local.tm_hour, local.tm_min);
    } else {
        snprintf(text, size, "in %lld s", (long long)(t - now));
    }
}

std::int64_t earliest(std::int64_t a, std::int64_t b)
{
    if (a == schedule::kNever) {
        return b;
    }
    if (b == schedule::kNever) {
        return a;
    }
    return a < b ? a : b;
}

#if CONFIG_POWER_DEEP_SLEEP
[[noreturn]] void sleep_for(std::int64_t sleep_s)
{
    ESP_LOGI(TAG, "Deep sleep for %lld s", (long long)sleep_s);
    schedule::prepare_sleep(sleep_s);
#if CONFIG_SD_LOG_ENABLE
    sd_log_flush(3000);
#endif
    // Radio off; the next boot starts it again.
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup((std::uint64_t)sleep_s * 1000000u);
    esp_deep_sleep_start();
}
#endif

void scheduler_task(void *arg)
{
    (void)arg;
    const bool woke = schedule::woke_from_sleep();
    const std::int64_t boot = schedule::now();
    // After a power-on the web installer and the console get some time.
    const std::int64_t start_at =
        woke ? boot : boot + CONFIG_SCHEDULE_START_DELAY_S;
#if CONFIG_POWER_DEEP_SLEEP
    const std::int64_t awake_until_boot =
        woke ? boot : boot + CONFIG_POWER_AWAKE_AFTER_BOOT_MIN * 60;
#endif

    {
        const schedule::Effective effective = schedule::effective();
        const schedule::Times times = schedule::next_times(effective);
        char camera[48];
        char checkin[48];
        describe(times.camera, boot, camera, sizeof(camera));
        describe(times.checkin, boot, checkin, sizeof(checkin));
        ESP_LOGI(TAG,
                 "Camera plan %s (%s), check-in every %u min (%s)%s; next "
                 "read %s, next check-in %s",
                 effective.camera_plan.c_str(),
                 schedule::origin_name(effective.camera_plan_origin),
                 (unsigned)effective.checkin_min,
                 schedule::origin_name(effective.checkin_origin),
                 effective.settings.uplink ? "" : " (no uplink settings)",
                 camera, checkin);
    }

    while (true) {
        if (s_battery_pending.exchange(false)) {
            const int battery = camera_protocol_last_battery_percent();
            if (battery >= 0) {
                schedule::set_camera_battery(battery);
            }
        }
        std::uint32_t wait_ms = SCHEDULER_MAX_WAIT_MS;
        session_status_t status;
        session_controller_get_status(&status);
        if (status.phase == SESSION_PHASE_IDLE) {
            const std::int64_t now = schedule::now();
            const schedule::Times times = schedule::next_times();
            const schedule::Task task = now >= start_at
                                            ? schedule::due(times, now)
                                            : schedule::Task::none;
            esp_err_t error = ESP_OK;
            if (task == schedule::Task::camera) {
                ESP_LOGI(TAG, "Camera read due");
                error = session_controller_request_scan();
            } else if (task == schedule::Task::checkin) {
                ESP_LOGI(TAG, "Backend check-in due");
                error = session_controller_request_upload();
            }
            if (task != schedule::Task::none) {
                if (error != ESP_OK) {
                    ESP_LOGW(TAG, "Could not start the cycle: %s",
                             esp_err_to_name(error));
                } else {
                    // The status listener wakes the task when it starts.
                    wait_ms = SCHEDULER_REQUEST_WAIT_MS;
                }
            } else {
                std::int64_t next = earliest(times.camera, times.checkin);
                if (now < start_at) {
                    next = earliest(next, start_at);
                }
#if CONFIG_POWER_DEEP_SLEEP
                std::int64_t awake_until = awake_until_boot;
                const std::int64_t activity = schedule::last_activity();
                if (activity != schedule::kNever &&
                    activity + CONFIG_POWER_AWAKE_AFTER_ACTIVITY_MIN * 60 >
                        awake_until) {
                    awake_until =
                        activity + CONFIG_POWER_AWAKE_AFTER_ACTIVITY_MIN * 60;
                }
                if (now >= awake_until) {
                    std::int64_t sleep_s = next == schedule::kNever
                                               ? CONFIG_POWER_MAX_SLEEP_MIN * 60
                                               : next - now;
                    if (sleep_s > CONFIG_POWER_MAX_SLEEP_MIN * 60) {
                        sleep_s = CONFIG_POWER_MAX_SLEEP_MIN * 60;
                    }
                    if (sleep_s >= CONFIG_POWER_MIN_SLEEP_S) {
                        char camera[48];
                        char checkin[48];
                        describe(times.camera, now, camera, sizeof(camera));
                        describe(times.checkin, now, checkin, sizeof(checkin));
                        ESP_LOGI(TAG, "Next read %s, next check-in %s", camera,
                                 checkin);
                        sleep_for(sleep_s);
                    }
                } else {
                    next = earliest(next, awake_until);
                }
#endif
                if (next != schedule::kNever) {
                    const std::int64_t left_ms = (next - now) * 1000;
                    if (left_ms < (std::int64_t)wait_ms) {
                        wait_ms =
                            left_ms < 1000 ? 1000 : (std::uint32_t)left_ms;
                    }
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

}  // namespace

esp_err_t scheduler_start(void)
{
    schedule::set_change_callback(wake_task);
    esp_err_t error = session_controller_add_status_listener(on_status, NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not follow the session status: %s",
                 esp_err_to_name(error));
        return error;
    }
    if (xTaskCreate(scheduler_task, "scheduler", SCHEDULER_TASK_STACK, NULL,
                    SCHEDULER_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
