#include "task_load.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace camera::diag
{

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS &&                                 \
    CONFIG_FREERTOS_USE_TRACE_FACILITY

namespace
{

constexpr std::size_t kMaxTasks = 40;

struct Sample {
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime;
};

// In PSRAM where available: internal RAM is short during a session.
TaskStatus_t *s_status;
Sample *s_start;
std::size_t s_start_count;
configRUN_TIME_COUNTER_TYPE s_start_total;

bool allocate()
{
    if (s_status == nullptr) {
        s_status = static_cast<TaskStatus_t *>(heap_caps_malloc(
            kMaxTasks * sizeof(TaskStatus_t), MALLOC_CAP_DEFAULT));
        s_start = static_cast<Sample *>(
            heap_caps_malloc(kMaxTasks * sizeof(Sample), MALLOC_CAP_DEFAULT));
    }
    return s_status != nullptr && s_start != nullptr;
}

}  // namespace

void task_load_start()
{
    s_start_count = 0;
    if (!allocate()) {
        return;
    }
    const UBaseType_t n =
        uxTaskGetSystemState(s_status, kMaxTasks, &s_start_total);
    for (UBaseType_t i = 0; i < n; ++i) {
        s_start[i] = {s_status[i].xHandle, s_status[i].ulRunTimeCounter};
    }
    s_start_count = n;
}

int task_load_report(char *out, std::size_t out_size, std::size_t count)
{
    if (out_size > 0) {
        out[0] = '\0';
    }
    if (s_start_count == 0) {
        return -1;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t n = uxTaskGetSystemState(s_status, kMaxTasks, &total);
    const configRUN_TIME_COUNTER_TYPE elapsed = total - s_start_total;
    if (elapsed == 0) {
        return -1;
    }
    // Replace each counter by the time since start() (a task started since
    // then counts from zero).
    configRUN_TIME_COUNTER_TYPE idle = 0;
    for (UBaseType_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < s_start_count; ++j) {
            if (s_start[j].handle == s_status[i].xHandle) {
                s_status[i].ulRunTimeCounter -= s_start[j].runtime;
                break;
            }
        }
        if (strncmp(s_status[i].pcTaskName, "IDLE", 4) == 0) {
            idle += s_status[i].ulRunTimeCounter;
        }
    }
    std::size_t used = 0;
    for (std::size_t k = 0; k < count && k < n; ++k) {
        UBaseType_t best = n;
        for (UBaseType_t i = 0; i < n; ++i) {
            if (strncmp(s_status[i].pcTaskName, "IDLE", 4) == 0) {
                continue;
            }
            if (best == n || s_status[i].ulRunTimeCounter >
                                 s_status[best].ulRunTimeCounter) {
                best = i;
            }
        }
        if (best == n || s_status[best].ulRunTimeCounter == 0) {
            break;
        }
        const unsigned percent = static_cast<unsigned>(
            (uint64_t)s_status[best].ulRunTimeCounter * 100 / elapsed);
        const int written =
            snprintf(out + used, out_size - used, "%s%s %u%%",
                     used > 0 ? ", " : "", s_status[best].pcTaskName, percent);
        if (written < 0 || (std::size_t)written >= out_size - used) {
            break;
        }
        used += written;
        s_status[best].ulRunTimeCounter = 0;
    }
    return static_cast<int>((uint64_t)idle * 100 / elapsed);
}

#else

void task_load_start()
{
}

int task_load_report(char *out, std::size_t out_size, std::size_t)
{
    if (out_size > 0) {
        out[0] = '\0';
    }
    return -1;
}

#endif

}  // namespace camera::diag
