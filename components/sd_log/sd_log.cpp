/*
 * This module tees the ESP-IDF log output into a ring buffer and writes it to
 * the SD card from a low-priority task (see sd_log.h).
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "log_files.hpp"
#include "sd_card.h"
#include "sd_log.h"

#define SD_LOG_DIR SD_CARD_MOUNT_POINT "/logs"
// Longer lines are cut; ESP-IDF log lines are well below this.
#define SD_LOG_LINE_MAX 256
#define SD_LOG_WRITE_CHUNK 4096
#define SD_LOG_FLUSH_INTERVAL_MS (CONFIG_SD_LOG_FLUSH_INTERVAL_S * 1000)
// The writer looks for new lines and flush requests this often.
#define SD_LOG_POLL_MS 250
#define SD_LOG_TASK_STACK 4096
#define SD_LOG_TASK_PRIORITY 2
#define SD_LOG_MAX_PATH 64
// One line per cycle (cycle_trace); the previous file is kept when it is
// full, so the two hold at least CONFIG_SD_LOG_CYCLES_MAX_KB of history.
#define SD_LOG_CYCLES_PATH SD_LOG_DIR "/cycles.jsonl"
#define SD_LOG_CYCLES_OLD_PATH SD_LOG_DIR "/cycles.1.jsonl"
// Lines waiting for the writer; more are dropped (a cycle line is ~300 B).
#define SD_LOG_CYCLES_PENDING_MAX 2048

static const char *TAG = "trailcamhub_sdlog";

namespace
{

vprintf_like_t s_console;
RingbufHandle_t s_ring;
std::atomic<bool> s_enabled;
std::atomic<unsigned> s_dropped;
SemaphoreHandle_t s_path_lock;
char s_path[SD_LOG_MAX_PATH];
unsigned s_file_number;
FILE *s_file;
size_t s_file_size;
// sd_log_flush(): set by the caller, answered by the writer.
std::atomic<bool> s_flush_requested;
SemaphoreHandle_t s_flushed;
// The running cycle (0: none): its number marks the lines in the file.
std::atomic<unsigned> s_cycle;
// The last chunk logged ended a line, so the next one starts one.
std::atomic<bool> s_line_start{true};
// Lines for cycles.jsonl, handed to the writer under s_path_lock.
std::string s_cycle_lines;

int log_vprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int result = s_console(format, args);
    if (s_enabled.load(std::memory_order_relaxed)) {
        char line[SD_LOG_LINE_MAX];
        // ESP-IDF logs a line with one call, so a call that follows a
        // complete line starts one. Lines of a cycle carry its number
        // ("#12 "), so that the file can be cut by cycle.
        const size_t prefix =
            s_line_start.load(std::memory_order_relaxed)
                ? sdlog::cycle_prefix(line, sizeof(line),
                                      s_cycle.load(std::memory_order_relaxed))
                : 0;
        const size_t length = sdlog::fit_line(
            line + prefix,
            vsnprintf(line + prefix, sizeof(line) - prefix, format, copy),
            sizeof(line) - prefix);
        if (length > 0) {
            s_line_start.store(line[prefix + length - 1] == '\n',
                               std::memory_order_relaxed);
            if (xRingbufferSend(s_ring, line, prefix + length, 0) != pdTRUE) {
                s_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    va_end(copy);
    return result;
}

void make_path(char *path, size_t size, unsigned number)
{
    snprintf(path, size, SD_LOG_DIR "/%s", sdlog::file_name(number).c_str());
}

// Removes the oldest files so that at most `keep` remain; returns the
// highest file number found.
unsigned prune_files(unsigned keep)
{
    std::vector<unsigned> numbers;
    unsigned highest = 0;
    DIR *dir = opendir(SD_LOG_DIR);
    if (dir == NULL) {
        return 0;
    }
    while (struct dirent *entry = readdir(dir)) {
        const unsigned number = sdlog::file_number(entry->d_name);
        if (number == 0) {
            continue;
        }
        numbers.push_back(number);
        if (number > highest) {
            highest = number;
        }
    }
    closedir(dir);
    // A file that cannot be removed is kept; no newer one is removed for it.
    char path[SD_LOG_MAX_PATH];
    for (unsigned number : sdlog::files_to_remove(std::move(numbers), keep)) {
        make_path(path, sizeof(path), number);
        remove(path);
    }
    return highest;
}

// Opens file `number` for appending.
bool open_file(unsigned number)
{
    char path[SD_LOG_MAX_PATH];
    make_path(path, sizeof(path), number);
    s_file = fopen(path, "a");
    if (s_file == NULL) {
        // Not logged: the message would only go into the buffer again.
        return false;
    }
    struct stat info;
    s_file_size = stat(path, &info) == 0 ? (size_t)info.st_size : 0;
    s_file_number = number;
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    memcpy(s_path, path, sizeof(s_path));
    xSemaphoreGive(s_path_lock);
    return true;
}

bool open_next_file()
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    // Keep one slot free for the new file.
    prune_files(CONFIG_SD_LOG_MAX_FILES - 1);
    return open_file(s_file_number + 1);
}

// After a wake-up from deep sleep the log goes on in the newest file, so a
// sleeping hub does not start a file per wake-up and push out the older ones.
bool continue_last_file(unsigned highest)
{
    if (highest == 0 || esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        return false;
    }
    char path[SD_LOG_MAX_PATH];
    make_path(path, sizeof(path), highest);
    struct stat info;
    if (stat(path, &info) != 0 ||
        (size_t)info.st_size >= CONFIG_SD_LOG_MAX_FILE_KB * 1024u) {
        return false;
    }
    return open_file(highest);
}

// Writes everything buffered so far and syncs it to the card.
void sync_file()
{
    if (s_file != NULL && (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0)) {
        // The data did not reach the card: continue in a new file.
        fclose(s_file);
        s_file = NULL;
    }
}

void write_bytes(const void *data, size_t size)
{
    if (s_file == NULL) {
        return;
    }
    if (fwrite(data, 1, size, s_file) != size) {
        // Card removed or full: stop writing until the next file.
        fclose(s_file);
        s_file = NULL;
        return;
    }
    s_file_size += size;
}

// Appends the waiting cycle lines to cycles.jsonl; the file is closed again,
// so the lines are on the card. A full file replaces the previous one.
void write_cycle_lines()
{
    std::string lines;
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    lines.swap(s_cycle_lines);
    xSemaphoreGive(s_path_lock);
    if (lines.empty()) {
        return;
    }
    struct stat info;
    if (stat(SD_LOG_CYCLES_PATH, &info) == 0 &&
        (size_t)info.st_size >= CONFIG_SD_LOG_CYCLES_MAX_KB * 1024u) {
        // FAT does not replace on rename.
        remove(SD_LOG_CYCLES_OLD_PATH);
        rename(SD_LOG_CYCLES_PATH, SD_LOG_CYCLES_OLD_PATH);
    }
    FILE *file = fopen(SD_LOG_CYCLES_PATH, "a");
    if (file == NULL) {
        return;
    }
    fwrite(lines.data(), 1, lines.size(), file);
    fclose(file);
}

void writer_task(void *arg)
{
    (void)arg;
    TickType_t last_flush = xTaskGetTickCount();
    bool dirty = false;
    while (true) {
        // Lines go to the file's buffer as they come; the card is synced
        // once per interval, or at once when a flush is requested (the wait
        // is short so that the request is seen quickly).
        size_t size = 0;
        void *data = xRingbufferReceiveUpTo(
            s_ring, &size, pdMS_TO_TICKS(SD_LOG_POLL_MS), SD_LOG_WRITE_CHUNK);
        if (data != NULL) {
            write_bytes(data, size);
            vRingbufferReturnItem(s_ring, data);
            dirty = true;
        }
        const bool flush_now = s_flush_requested.load();
        if (flush_now) {
            // Everything logged up to the request.
            while ((data = xRingbufferReceiveUpTo(
                        s_ring, &size, 0, SD_LOG_WRITE_CHUNK)) != NULL) {
                write_bytes(data, size);
                vRingbufferReturnItem(s_ring, data);
                dirty = true;
            }
        }
        write_cycle_lines();
        const unsigned dropped = s_dropped.exchange(0);
        if (dropped > 0) {
            char note[64];
            const int length = snprintf(note, sizeof(note),
                                        "[sd_log] %u lines dropped\n", dropped);
            write_bytes(note, length);
            dirty = true;
        }
        const TickType_t now = xTaskGetTickCount();
        if (!flush_now &&
            now - last_flush < pdMS_TO_TICKS(SD_LOG_FLUSH_INTERVAL_MS)) {
            continue;
        }
        last_flush = now;
        if (dirty) {
            sync_file();
            dirty = false;
            if (s_file == NULL ||
                s_file_size >= CONFIG_SD_LOG_MAX_FILE_KB * 1024u) {
                open_next_file();
            }
        }
        if (flush_now) {
            s_flush_requested = false;
            xSemaphoreGive(s_flushed);
        }
    }
}

}  // namespace

esp_err_t sd_log_init(void)
{
    s_path_lock = xSemaphoreCreateMutex();
    s_flushed = xSemaphoreCreateBinary();
    if (s_path_lock == NULL || s_flushed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Prefer PSRAM; without it a smaller buffer in internal RAM.
    s_ring = xRingbufferCreateWithCaps(CONFIG_SD_LOG_BUFFER_KB * 1024,
                                       RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        s_ring = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_BYTEBUF);
    }
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_enabled = true;
    s_console = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

esp_err_t sd_log_start(void)
{
    if (s_ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(SD_LOG_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Could not create %s: errno %d", SD_LOG_DIR, errno);
        sd_log_disable();
        return ESP_FAIL;
    }
    s_file_number = prune_files(CONFIG_SD_LOG_MAX_FILES);
    if (!continue_last_file(s_file_number) && !open_next_file()) {
        ESP_LOGW(TAG, "Could not open a log file in %s", SD_LOG_DIR);
        sd_log_disable();
        return ESP_FAIL;
    }
    if (xTaskCreate(writer_task, "sd_log", SD_LOG_TASK_STACK, NULL,
                    SD_LOG_TASK_PRIORITY, NULL) != pdPASS) {
        sd_log_disable();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Writing the log to %s", s_path);
    return ESP_OK;
}

void sd_log_disable(void)
{
    s_enabled = false;
}

void sd_log_current_path(char *path, size_t size)
{
    if (size == 0) {
        return;
    }
    path[0] = '\0';
    if (s_path_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    snprintf(path, size, "%s", s_path);
    xSemaphoreGive(s_path_lock);
}

void sd_log_set_cycle(uint32_t cycle)
{
    s_cycle.store(cycle, std::memory_order_relaxed);
}

void sd_log_append_cycle_line(const char *line)
{
    if (!s_enabled.load() || s_path_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_path_lock, portMAX_DELAY);
    if (s_cycle_lines.size() + strlen(line) <= SD_LOG_CYCLES_PENDING_MAX) {
        s_cycle_lines += line;
    }
    xSemaphoreGive(s_path_lock);
}

esp_err_t sd_log_flush(uint32_t timeout_ms)
{
    if (!s_enabled.load() || s_flushed == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_flushed, 0);  // an old answer
    s_flush_requested = true;
    return xSemaphoreTake(s_flushed, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}
