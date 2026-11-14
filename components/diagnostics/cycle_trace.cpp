/*
 * Numbers the cycles, samples the free heap while one runs and writes the
 * cycle journal (see cycle_trace.hpp). The formats are in cycle_record.cpp.
 */

#include "cycle_trace.hpp"

#include <atomic>
#include <ctime>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cycle_record.hpp"
#include "system_diagnostics.h"
#if CONFIG_SD_LOG_ENABLE
#include "sd_log.h"
#endif

namespace cycle_trace
{

namespace
{

const char *TAG = "trailcamhub_cycle";

// The free heap is sampled this often while a cycle runs.
constexpr std::uint64_t kSampleIntervalUs = 1000000;
// A clock before this is not set (2023-11-14).
constexpr std::int64_t kClockValidAfter = 1700000000;
// The internal heap that data can come from. Plain MALLOC_CAP_INTERNAL also
// counts heaps that cannot hold byte buffers: about 20 KB on the classic
// ESP32, where an allocation failed although that figure showed 28 KB as
// the minimum (#244).
constexpr std::uint32_t kDataHeap = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr std::uint32_t kNumberMagic = 0x43594331;  // "CYC1"

// RTC memory that the start-up code leaves alone: the number goes on after
// deep sleep, a crash and a restart. After a power loss the magic is gone
// and the count starts again.
RTC_NOINIT_ATTR std::uint32_t s_number_magic;
RTC_NOINIT_ATTR std::uint32_t s_number;

SemaphoreHandle_t s_lock;
esp_timer_handle_t s_timer;
// Guarded by s_lock.
bool s_running;
Record s_current;
bool s_has_last;
Record s_last;
std::string s_next_trigger;
std::int64_t s_started_us;
std::int64_t s_boot_min_internal_at_start;
std::int64_t s_boot_min_psram_at_start;
// Lowest samples of the running cycle (timer task and cycle start).
std::atomic<std::int64_t> s_sample_internal;
std::atomic<std::int64_t> s_sample_psram;

bool has_psram()
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

void lower(std::atomic<std::int64_t> &lowest, std::int64_t value)
{
    std::int64_t seen = lowest.load();
    while (value < seen && !lowest.compare_exchange_weak(seen, value)) {
    }
}

void sample(void *arg)
{
    (void)arg;
    lower(s_sample_internal, (std::int64_t)heap_caps_get_free_size(kDataHeap));
    lower(s_sample_psram,
          (std::int64_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

std::int64_t heap_min_internal()
{
    return cycle_heap_minimum(
        s_boot_min_internal_at_start,
        (std::int64_t)heap_caps_get_minimum_free_size(kDataHeap),
        s_sample_internal.load());
}

std::int64_t heap_min_psram()
{
    if (!has_psram()) {
        return -1;
    }
    return cycle_heap_minimum(
        s_boot_min_psram_at_start,
        (std::int64_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        s_sample_psram.load());
}

// The tasks of the session controller live as long as the firmware runs.
std::int32_t stack_minimum(const char *task_name)
{
    const TaskHandle_t task = xTaskGetHandle(task_name);
    return task == NULL ? -1 : (std::int32_t)uxTaskGetStackHighWaterMark(task);
}

StackMinimum stack_minimums()
{
    StackMinimum stack;
    stack.session = stack_minimum("camera_session");
    stack.uplink = stack_minimum("uplink");
    stack.controller = stack_minimum("session_ctrl");
    return stack;
}

std::int64_t wall_clock()
{
    const std::int64_t now = (std::int64_t)time(nullptr);
    return now > kClockValidAfter ? now : 0;
}

void append_line(const std::string &line)
{
#if CONFIG_SD_LOG_ENABLE
    sd_log_append_cycle_line(line.c_str());
#else
    (void)line;
#endif
}

}  // namespace

void init()
{
    if (s_number_magic != kNumberMagic) {
        s_number_magic = kNumberMagic;
        s_number = 0;
    }
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t timer_args = {
        .callback = sample,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cycle_heap",
        .skip_unhandled_events = true,
    };
    if (s_lock == NULL || esp_timer_create(&timer_args, &s_timer) != ESP_OK) {
        // The cycles then run without a trace.
        ESP_LOGW(TAG, "Could not set up the cycle trace");
        s_lock = NULL;
        return;
    }
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_DEEPSLEEP) {
        ESP_LOGI(TAG, "Last cycle before this start: %u", (unsigned)s_number);
        append_line(boot_line(diagnostics_reset_reason_name((int)reason),
                              s_number, wall_clock()));
    }
}

void note_trigger(const char *trigger)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_next_trigger = trigger == nullptr ? "" : trigger;
    xSemaphoreGive(s_lock);
}

void on_cycle_started(const char *kind)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_number += 1;
    s_current = Record{};
    s_current.number = s_number;
    s_current.start = wall_clock();
    s_current.kind = kind;
    s_current.trigger = s_next_trigger.empty() ? "request" : s_next_trigger;
    s_next_trigger.clear();
    s_started_us = esp_timer_get_time();
    s_boot_min_internal_at_start =
        (std::int64_t)heap_caps_get_minimum_free_size(kDataHeap);
    s_boot_min_psram_at_start =
        (std::int64_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s_sample_internal = (std::int64_t)heap_caps_get_free_size(kDataHeap);
    s_sample_psram = (std::int64_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_running = true;
    const Record started = s_current;
    xSemaphoreGive(s_lock);

#if CONFIG_SD_LOG_ENABLE
    sd_log_set_cycle(started.number);
#endif
    esp_timer_start_periodic(s_timer, kSampleIntervalUs);
    ESP_LOGI(TAG, "Cycle %u started: %s (%s)", (unsigned)started.number,
             started.kind.c_str(), started.trigger.c_str());
}

void on_cycle_finished(const char *result, std::uint32_t cameras_done,
                       std::uint32_t cameras_read)
{
    if (s_lock == NULL) {
        return;
    }
    esp_timer_stop(s_timer);
    sample(nullptr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_current.result = result;
    s_current.duration_s =
        (std::uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
    s_current.cameras_done = cameras_done;
    s_current.cameras_read = cameras_read;
    s_current.heap_min_internal = heap_min_internal();
    s_current.heap_min_psram = heap_min_psram();
    s_current.stack = stack_minimums();
    s_last = s_current;
    s_has_last = true;
    s_running = false;
    const Record finished = s_last;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%s", describe(finished).c_str());
    append_line(cycle_line(finished));
#if CONFIG_SD_LOG_ENABLE
    sd_log_set_cycle(0);
#endif
}

Memory memory()
{
    Memory out;
    if (s_lock == NULL) {
        return out;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        out.cycle = s_current.number;
        out.heap_min_internal = heap_min_internal();
        out.heap_min_psram = heap_min_psram();
    } else if (s_has_last) {
        out.cycle = s_last.number;
        out.heap_min_internal = s_last.heap_min_internal;
        out.heap_min_psram = s_last.heap_min_psram;
    } else {
        out.heap_min_internal =
            (std::int64_t)heap_caps_get_minimum_free_size(kDataHeap);
        out.heap_min_psram =
            has_psram() ? (std::int64_t)heap_caps_get_minimum_free_size(
                              MALLOC_CAP_SPIRAM)
                        : -1;
    }
    xSemaphoreGive(s_lock);
    const StackMinimum stack = stack_minimums();
    out.stack_session = stack.session;
    out.stack_uplink = stack.uplink;
    out.stack_controller = stack.controller;
    return out;
}

std::string status_text()
{
    if (s_lock == NULL) {
        return "no cycle trace";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::string text = s_has_last ? describe(s_last) : "no cycle yet";
    if (s_running) {
        text += "; cycle " + std::to_string(s_current.number) + " " +
                s_current.kind + " (" + s_current.trigger + ") runs";
    }
    xSemaphoreGive(s_lock);
    return text;
}

}  // namespace cycle_trace
