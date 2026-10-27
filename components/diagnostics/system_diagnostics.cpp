/*
 * This module reports boot information and heap usage so that memory
 * headroom can be measured on the target board during real camera sessions.
 */

#include <stddef.h>

#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "system_diagnostics.h"

#ifndef CONFIG_DIAG_MEMORY_LOG_INTERVAL_S
#error "main/Kconfig.projbuild was not processed"
#endif

static const char *TAG = "trailcamhub_diag";

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software restart";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep wake-up";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_USB:
        return "USB";
    default:
        return "other";
    }
}

static void log_core_dump_status(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) == ESP_OK) {
        ESP_LOGW(TAG,
                 "Core dump from a previous crash is stored in flash "
                 "(%u bytes); read it with idf.py coredump-info",
                 (unsigned)size);
    }
#endif
}

static void periodic_memory_log(void *arg)
{
    (void)arg;
    diagnostics_log_memory("periodic");
}

void diagnostics_log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "Memory [%s]: internal free %u B (min %u B, largest block %u B), "
             "PSRAM free %u B (min %u B)",
             stage, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t diagnostics_start(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %s (%d)", reset_reason_name(reason),
             (int)reason);
    log_core_dump_status();

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size == 0) {
        ESP_LOGW(TAG, "No PSRAM available to the heap");
    } else {
        ESP_LOGI(TAG, "PSRAM available to the heap: %u KB",
                 (unsigned)(psram_size / 1024));
    }
    diagnostics_log_memory("boot");

#if CONFIG_DIAG_MEMORY_LOG_INTERVAL_S > 0
    const esp_timer_create_args_t timer_args = {
        .callback = periodic_memory_log,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "memory_log",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG,
                        "Could not create memory log timer");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(
            timer, (uint64_t)CONFIG_DIAG_MEMORY_LOG_INTERVAL_S * 1000000ULL),
        TAG, "Could not start memory log timer");
#else
    (void)periodic_memory_log;
#endif
    return ESP_OK;
}
