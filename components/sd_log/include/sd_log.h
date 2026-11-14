#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Copies the log output to text files on the SD card, so that a test run can
 * be read afterwards without a serial console. Lines are collected in a ring
 * buffer and written by a background task, which syncs the file to the card
 * every CONFIG_SD_LOG_FLUSH_INTERVAL_S seconds, so pulling the device loses at
 * most that interval.
 *
 * Files are /sdcard/logs/log-NNNNN.txt with a number that increases with every
 * boot and when a file reaches its size limit; the oldest files are removed.
 * A wake-up from deep sleep continues the newest file.
 *
 * /sdcard/logs/cycles.jsonl has one line per cycle (docs/sd-layout.md); when
 * it reaches CONFIG_SD_LOG_CYCLES_MAX_KB it becomes cycles.1.jsonl.
 */

/*
 * Starts capturing log lines into RAM. Call first in app_main so that the
 * boot messages are kept until the card is mounted.
 */
esp_err_t sd_log_init(void);

/* Opens the first log file and starts writing; call after the card is up. */
esp_err_t sd_log_start(void);

/*
 * Writes the lines logged so far and syncs the file, for example before deep
 * sleep. Waits at most `timeout_ms`.
 */
esp_err_t sd_log_flush(uint32_t timeout_ms);

/* Stops capturing, for example when no card could be mounted. */
void sd_log_disable(void);

/*
 * Copies the path of the file being written to `path` (empty if none).
 * Safe to call from any task.
 */
void sd_log_current_path(char *path, size_t size);

/*
 * Marks the log lines written from now on with the number of the running
 * cycle ("#12 " in front of the line in the file); 0 ends the marking.
 */
void sd_log_set_cycle(uint32_t cycle);

/*
 * Appends a line (ending with a newline) to cycles.jsonl; written by the log
 * task within a moment and before sd_log_flush() returns. Dropped when the
 * log is not written. Safe to call from any task.
 */
void sd_log_append_cycle_line(const char *line);
