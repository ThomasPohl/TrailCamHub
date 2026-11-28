#include "host_cycle.hpp"

#include <sys/time.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "time_sync.h"

namespace
{

std::int64_t s_wall_us;
std::int64_t s_boot_wall_us;
esp_reset_reason_t s_reset_reason = ESP_RST_POWERON;
std::vector<std::string> s_log;

// namespace -> key -> bytes. Strings are stored with their terminator.
std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> s_nvs;
struct OpenHandle {
    std::string name;
    bool writable;
};
std::map<nvs_handle_t, OpenHandle> s_handles;
nvs_handle_t s_next_handle = 1;

}  // namespace

struct HostSemaphore {
    std::recursive_mutex mutex;
};

namespace
{
// Kept for the whole run, so that a semaphore of an earlier boot stays
// reachable (no leak report) and its address stays valid.
std::deque<HostSemaphore> s_semaphores;
}  // namespace

namespace host_cycle
{

void power_on(std::int64_t unix_s)
{
    s_nvs.clear();
    s_handles.clear();
    s_wall_us = unix_s * 1000000;
    s_boot_wall_us = s_wall_us;
    s_reset_reason = ESP_RST_POWERON;
}

void restart()
{
    s_boot_wall_us = s_wall_us;
    s_reset_reason = ESP_RST_SW;
}

void deep_sleep(std::int64_t sleep_s)
{
    s_wall_us += sleep_s * 1000000;
    s_boot_wall_us = s_wall_us;
    s_reset_reason = ESP_RST_DEEPSLEEP;
}

void advance_ms(std::int64_t ms)
{
    s_wall_us += ms * 1000;
}

std::int64_t wall_unix()
{
    return s_wall_us / 1000000;
}

std::int64_t uptime_us()
{
    return s_wall_us - s_boot_wall_us;
}

std::vector<std::string> nvs_keys()
{
    std::vector<std::string> keys;
    for (const auto &space : s_nvs) {
        for (const auto &entry : space.second) {
            keys.push_back(space.first + "/" + entry.first);
        }
    }
    return keys;
}

void erase_nvs()
{
    s_nvs.clear();
}

std::vector<std::string> log_lines()
{
    return s_log;
}

void clear_log()
{
    s_log.clear();
}

}  // namespace host_cycle

namespace
{

std::map<std::string, std::vector<std::uint8_t>> *space_of(nvs_handle_t handle,
                                                           bool write)
{
    const auto open = s_handles.find(handle);
    if (open == s_handles.end() || (write && !open->second.writable)) {
        return nullptr;
    }
    return &s_nvs[open->second.name];
}

esp_err_t get_bytes(nvs_handle_t handle, const char *key, void *out,
                    size_t *length)
{
    const auto *space = space_of(handle, false);
    if (space == nullptr) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    const auto entry = space->find(key);
    if (entry == space->end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out == nullptr) {
        *length = entry->second.size();
        return ESP_OK;
    }
    if (*length < entry->second.size()) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::memcpy(out, entry->second.data(), entry->second.size());
    *length = entry->second.size();
    return ESP_OK;
}

esp_err_t set_bytes(nvs_handle_t handle, const char *key, const void *value,
                    size_t length)
{
    auto *space = space_of(handle, true);
    if (space == nullptr) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(value);
    (*space)[key].assign(bytes, bytes + length);
    return ESP_OK;
}

template <typename T>
esp_err_t get_value(nvs_handle_t handle, const char *key, T *out)
{
    size_t length = sizeof(T);
    std::uint8_t bytes[sizeof(T)];
    const esp_err_t error = get_bytes(handle, key, bytes, &length);
    if (error == ESP_OK) {
        if (length != sizeof(T)) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        std::memcpy(out, bytes, sizeof(T));
    }
    return error;
}

}  // namespace

extern "C" {

// Linked with --wrap: the components' wall clock is the virtual one.
int __wrap_gettimeofday(struct timeval *tv, void *)
{
    if (tv != nullptr) {
        tv->tv_sec = s_wall_us / 1000000;
        tv->tv_usec = s_wall_us % 1000000;
    }
    return 0;
}

time_t __wrap_time(time_t *out)
{
    const time_t now = static_cast<time_t>(s_wall_us / 1000000);
    if (out != nullptr) {
        *out = now;
    }
    return now;
}

int64_t esp_timer_get_time(void)
{
    return host_cycle::uptime_us();
}

esp_reset_reason_t esp_reset_reason(void)
{
    return s_reset_reason;
}

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

void host_log_line(int, const char *tag, const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    s_log.push_back(std::string(tag) + ": " + text);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_semaphores.emplace_back();
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return &s_semaphores.emplace_back();
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t)
{
    semaphore->mutex.lock();
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->mutex.unlock();
    return pdTRUE;
}

int xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    return xSemaphoreTake(semaphore, ticks);
}

int xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    return xSemaphoreGive(semaphore);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    // Like the chip: a namespace that was never written cannot be opened
    // read-only.
    if (mode == NVS_READONLY && s_nvs.find(name) == s_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = s_next_handle++;
    s_handles[*out] = OpenHandle{name, mode == NVS_READWRITE};
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    s_handles.erase(handle);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return s_handles.count(handle) != 0 ? ESP_OK : ESP_ERR_NVS_INVALID_HANDLE;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length)
{
    return get_bytes(handle, key, out, length);
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return set_bytes(handle, key, value, std::strlen(value) + 1);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length)
{
    return get_bytes(handle, key, out, length);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    return set_bytes(handle, key, value, length);
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    return get_value(handle, key, out);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return set_bytes(handle, key, &value, sizeof(value));
}

esp_err_t nvs_get_i64(nvs_handle_t handle, const char *key, int64_t *out)
{
    return get_value(handle, key, out);
}

esp_err_t nvs_set_i64(nvs_handle_t handle, const char *key, int64_t value)
{
    return set_bytes(handle, key, &value, sizeof(value));
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    auto *space = space_of(handle, true);
    if (space == nullptr) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    return space->erase(key) != 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    auto *space = space_of(handle, true);
    if (space == nullptr) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    space->clear();
    return ESP_OK;
}

// The clock is set (as after SNTP); the camera time is not used here.
bool time_sync_clock_valid(void)
{
    return true;
}

bool time_sync_trusted(void)
{
    return true;
}

bool time_sync_set(int64_t, time_sync_source_t)
{
    return false;
}

}  // extern "C"
