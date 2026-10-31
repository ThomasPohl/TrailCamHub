#pragma once

#include <stddef.h>

#include "esp_err.h"

/*
 * Copies the log output to text files on the SD card, so that a test run can
 * be read afterwards without a serial console. Lines are collected in a ring
 * buffer and written by a background task about once per second; each write
 * is synced to the card, so pulling the device loses at most that second.
 *
 * Files are /sdcard/logs/log-NNNNN.txt with a number that increases with every
 * boot and when a file reaches its size limit; the oldest files are removed.
 */

/*
 * Starts capturing log lines into RAM. Call first in app_main so that the
 * boot messages are kept until the card is mounted.
 */
esp_err_t sd_log_init(void);

/* Opens the first log file and starts writing; call after the card is up. */
esp_err_t sd_log_start(void);

/* Stops capturing, for example when no card could be mounted. */
void sd_log_disable(void);

/*
 * Copies the path of the file being written to `path` (empty if none).
 * Safe to call from any task.
 */
void sd_log_current_path(char *path, size_t size);
