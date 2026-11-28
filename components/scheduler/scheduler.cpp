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
#if CONFIG_HUB_BOOT_BUTTON
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#endif
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cycle_trace.hpp"
#include "hub_battery.hpp"
#include "modem_uplink.hpp"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "scheduler_step.hpp"
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

schedule::StepLimits make_limits()
{
    schedule::StepLimits limits;
#if CONFIG_POWER_DEEP_SLEEP
    limits.deep_sleep = true;
    limits.awake_after_activity_s = CONFIG_POWER_AWAKE_AFTER_ACTIVITY_MIN * 60;
    limits.min_sleep_s = CONFIG_POWER_MIN_SLEEP_S;
    limits.max_sleep_s = CONFIG_POWER_MAX_SLEEP_MIN * 60;
#endif
    limits.max_wait_ms = SCHEDULER_MAX_WAIT_MS;
    limits.request_wait_ms = SCHEDULER_REQUEST_WAIT_MS;
    return limits;
}

const schedule::StepLimits kLimits = make_limits();

TaskHandle_t s_task;
// Only touched by the session controller task (status listener).
session_phase_t s_last_phase = SESSION_PHASE_IDLE;
bool s_session_reached;
// Lowest battery of the cameras read in the last cycle; -1: none to store.
std::atomic<int> s_pending_battery{-1};
int s_cycle_battery = -1;

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
    // moves the next read nor answers a read that the backend asked for. The
    // same holds for a short session that only runs queued actions.
    if (status->list_only || status->actions_only) {
        s_last_phase = status->phase;
        wake_task();
        return;
    }
    if (s_last_phase == SESSION_PHASE_IDLE &&
        status->phase != SESSION_PHASE_IDLE) {
        schedule::on_cycle_started(camera_phase(status->phase));
        s_session_reached = false;
        s_cycle_battery = -1;
    }
    if (status->phase == SESSION_PHASE_CAMERA_SESSION) {
        s_session_reached = true;
    }
    // Each camera read answers the read requests for it, and its battery
    // counts for the low-battery stretch (the weakest camera decides).
    if (status->camera_finished && status->cameras_done >= 1 &&
        status->cameras_done <= SESSION_MAX_CAMERA_RESULTS) {
        const session_camera_t &camera =
            status->cameras[status->cameras_done - 1];
        if ((camera.result == SESSION_CAMERA_READ ||
             camera.result == SESSION_CAMERA_PARTIAL) &&
            camera.cam_id[0] != '\0') {
            schedule::on_camera_read(camera.cam_id);
        }
        if (camera.bat_percent >= 0 && camera.bat_percent <= 100 &&
            (s_cycle_battery < 0 || camera.bat_percent < s_cycle_battery)) {
            s_cycle_battery = camera.bat_percent;
        }
    }
    const bool stopped =
        status->cycle_finished && status->last_result == SESSION_RESULT_STOPPED;
    // The camera part ends with the start of the sync or with the cycle. The
    // read requests of cameras it did not read have failed then.
    if (camera_phase(s_last_phase) && !camera_phase(status->phase) &&
        !stopped) {
        schedule::on_camera_part_ended();
        if (s_cycle_battery >= 0) {
            s_pending_battery = s_cycle_battery;
        }
    }
    if (status->cycle_finished) {
        schedule::CycleEnd end;
        // A camera was read (fully or in part). A session that ended with an
        // error before storing anything, or found no SD card, is no read.
        end.camera_ok = s_session_reached && status->camera_ok && !stopped;
        // Backs off the next reads when no camera was read; a camera without
        // an SD card would only spend its battery on them.
        end.camera_failed =
            status->last_result == SESSION_RESULT_CAMERA_UNREACHABLE ||
            status->last_result == SESSION_RESULT_CAMERA_NO_CARD ||
            (status->camera_no_card && status->cameras_read == 0 && !stopped);
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

// Reads the hub battery for its power level (only between cycles, so that
// the modem's load does not pull the voltage down).
void update_hub_battery()
{
    if (!hub_battery::supported()) {
        return;
    }
    hub_battery::Reading reading;
    if (hub_battery::read(reading)) {
        schedule::set_hub_battery(reading.percent, reading.millivolts,
                                  reading.charging);
    } else {
        schedule::set_hub_battery(-1, -1, false);
    }
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

// The modem is taken down after a sync in its own task (data call ended,
// radio off); sleeping or restarting before that would leave its radio on.
void wait_for_modem_down(const char *what)
{
    for (int i = 0; i < 300 && modem_uplink::active(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (modem_uplink::active()) {
        ESP_LOGW(TAG, "Modem still active; %s anyway", what);
    }
}

#if CONFIG_POWER_DEEP_SLEEP
[[noreturn]] void sleep_for(std::int64_t sleep_s)
{
    wait_for_modem_down("sleeping");
    ESP_LOGI(TAG, "Deep sleep for %lld s", (long long)sleep_s);
    schedule::prepare_sleep(sleep_s);
#if CONFIG_SD_LOG_ENABLE
    sd_log_flush(3000);
#endif
    // Radio off; the next boot starts it again.
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup((std::uint64_t)sleep_s * 1000000u);
#if CONFIG_HUB_BOOT_BUTTON
    // The button also wakes the hub (#224); it pulls the pin to ground.
    const gpio_num_t button = static_cast<gpio_num_t>(CONFIG_HUB_BUTTON_GPIO);
    if (esp_sleep_is_valid_wakeup_gpio(button)) {
        rtc_gpio_pullup_en(button);
        rtc_gpio_pulldown_dis(button);
        esp_sleep_enable_ext0_wakeup(button, 0);
    } else {
        ESP_LOGW(TAG, "GPIO %d cannot wake the hub from deep sleep",
                 CONFIG_HUB_BUTTON_GPIO);
    }
#endif
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

#if CONFIG_HUB_BOOT_BUTTON
    // A press of the button woke the hub: read the cameras at once (#224).
    bool button_wake =
        woke && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    if (button_wake) {
        ESP_LOGI(TAG, "Woken by the button");
    }
#endif
    update_hub_battery();
    {
        const schedule::Effective effective = schedule::effective();
        const schedule::Times times = schedule::next_times(effective);
        char camera[48];
        char checkin[48];
        describe(times.camera, boot, camera, sizeof(camera));
        describe(times.checkin, boot, checkin, sizeof(checkin));
        ESP_LOGI(TAG,
                 "Camera plan %s (%s), quiet hours %s, check-in every %u min "
                 "(%s)%s; next read %s, next check-in %s",
                 effective.camera_plan.c_str(),
                 schedule::origin_name(effective.camera_plan_origin),
                 effective.quiet.c_str(), (unsigned)effective.checkin_min,
                 schedule::origin_name(effective.checkin_origin),
                 effective.settings.uplink ? "" : " (no uplink settings)",
                 camera, checkin);
    }

    while (true) {
        const int battery = s_pending_battery.exchange(-1);
        if (battery >= 0) {
            schedule::set_camera_battery(battery);
        }
        std::uint32_t wait_ms = SCHEDULER_MAX_WAIT_MS;
        session_status_t status;
        session_controller_get_status(&status);
        if (status.phase == SESSION_PHASE_IDLE && ota::restart_pending()) {
            // A new firmware was installed during the last sync.
            wait_for_modem_down("restarting");
            ota::restart();
        }
        if (status.phase == SESSION_PHASE_IDLE) {
            update_hub_battery();
            const std::int64_t now = schedule::now();
            const schedule::Times times = schedule::next_times();
            esp_err_t error = ESP_OK;
#if CONFIG_HUB_BOOT_BUTTON
            if (button_wake) {
                button_wake = false;
                // Whatever is due, the person at the hub wants the cameras.
                cycle_trace::note_trigger("button");
                error = session_controller_request_scan();
                if (error != ESP_OK) {
                    cycle_trace::note_trigger(nullptr);
                    ESP_LOGW(TAG, "Could not start the cycle: %s",
                             esp_err_to_name(error));
                } else {
                    wait_ms = SCHEDULER_REQUEST_WAIT_MS;
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
                continue;
            }
#endif
            schedule::StepInput input;
            input.now = now;
            input.times = times;
            input.start_at = start_at;
#if CONFIG_POWER_DEEP_SLEEP
            input.awake_until_boot = awake_until_boot;
            input.last_activity = schedule::last_activity();
            // Only on battery power (the mode can change at run time).
            input.sleeps = [](void *) { return schedule::effective().sleeps; };
#endif
            const schedule::Step step =
                schedule::scheduler_step(input, kLimits);
            if (step.action == schedule::Step::Action::camera ||
                step.action == schedule::Step::Action::checkin) {
                // The cycle journal names who asked for a cycle.
                cycle_trace::note_trigger("schedule");
                if (step.action == schedule::Step::Action::camera) {
                    ESP_LOGI(TAG, "Camera read due");
                    error = session_controller_request_scan();
                } else {
                    ESP_LOGI(TAG, "Backend check-in due");
                    error = session_controller_request_upload();
                }
                if (error != ESP_OK) {
                    cycle_trace::note_trigger(nullptr);
                    ESP_LOGW(TAG, "Could not start the cycle: %s",
                             esp_err_to_name(error));
                } else {
                    // The status listener wakes the task when it starts.
                    wait_ms = step.wait_ms;
                }
            } else if (step.action == schedule::Step::Action::sleep) {
#if CONFIG_POWER_DEEP_SLEEP
                char camera[48];
                char checkin[48];
                describe(times.camera, now, camera, sizeof(camera));
                describe(times.checkin, now, checkin, sizeof(checkin));
                ESP_LOGI(TAG, "Next read %s, next check-in %s", camera,
                         checkin);
                sleep_for(step.sleep_s);
#endif
            } else {
                wait_ms = step.wait_ms;
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
