#include "host_esp.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sd_writer.hpp"
#include "time_sync.h"
#ifdef HOST_VIRTUAL_LINK
#include "virtual_net.hpp"
#endif

namespace
{

std::mutex s_log_lock;
std::vector<host_esp::LogLine> s_log;
bool s_clock_valid = true;

}  // namespace

extern "C" {

int64_t esp_timer_get_time(void)
{
#ifdef HOST_VIRTUAL_LINK
    return virtual_net::now_us();
#else
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
        .count();
#endif
}

uint32_t esp_random(void)
{
    return static_cast<uint32_t>(std::rand());
}

void esp_fill_random(void *buf, size_t len)
{
    auto *bytes = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; ++i) {
        bytes[i] = static_cast<uint8_t>(std::rand());
    }
}

size_t heap_caps_get_largest_free_block(unsigned)
{
    return 100000;
}

size_t heap_caps_get_free_size(unsigned)
{
    return 200000;
}

size_t heap_caps_get_minimum_free_size(unsigned)
{
    return 150000;
}

void host_task_delay_ms(uint32_t ms)
{
#ifdef HOST_VIRTUAL_LINK
    virtual_net::advance_us(static_cast<int64_t>(ms) * 1000);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = {"host", "trailcamhub", "", ""};
    return &desc;
}

void host_log_line(int level, const char *tag, const char *format, ...)
{
    char text[600];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::lock_guard<std::mutex> guard(s_log_lock);
    s_log.push_back({level, tag, text});
    if (std::getenv("HOST_SESSION_LOG") != nullptr) {
        std::fprintf(stderr, "%d %s: %s\n", level, tag, text);
    }
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = ((slen + 2) / 3) * 4 + 1;
    if (dlen < need) {
        *olen = need;
        return -0x002A;
    }
    size_t out = 0;
    for (size_t i = 0; i < slen; i += 3) {
        const uint32_t b0 = src[i];
        const uint32_t b1 = i + 1 < slen ? src[i + 1] : 0;
        const uint32_t b2 = i + 2 < slen ? src[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        dst[out++] = static_cast<unsigned char>(kAlphabet[(v >> 18) & 63]);
        dst[out++] = static_cast<unsigned char>(kAlphabet[(v >> 12) & 63]);
        const char c2 = i + 1 < slen ? kAlphabet[(v >> 6) & 63] : '=';
        const char c3 = i + 2 < slen ? kAlphabet[v & 63] : '=';
        dst[out++] = static_cast<unsigned char>(c2);
        dst[out++] = static_cast<unsigned char>(c3);
    }
    dst[out] = 0;
    *olen = out;
    return 0;
}

bool time_sync_clock_valid(void)
{
    return s_clock_valid;
}

bool time_sync_trusted(void)
{
    return true;
}

bool time_sync_set(int64_t, time_sync_source_t)
{
    s_clock_valid = true;
    return true;
}

}  // extern "C"

namespace camera::store
{

// The SD writer without its task: every write goes to the file directly, as
// on a board without PSRAM.
bool sd_writer_start()
{
    return false;
}
bool sd_writer_begin_session()
{
    return false;
}
void sd_writer_end_session()
{
}
bool sd_writer_active()
{
    return false;
}
bool sd_writer_has_room(std::size_t)
{
    return false;
}
bool sd_writer_write(MediaFileWriter &file, const std::uint8_t *data,
                     std::size_t size)
{
    return file.write(data, size);
}
bool sd_writer_flush(bool)
{
    return true;
}
std::uint32_t sd_writer_end_files(MediaFileWriter *, MediaFileWriter *)
{
    return 1;
}
bool sd_writer_ended(std::uint32_t, bool &ok)
{
    ok = true;
    return true;
}
void sd_writer_take_stats(std::int64_t &busy_us, std::int64_t &longest_us)
{
    busy_us = 0;
    longest_us = 0;
}

}  // namespace camera::store

namespace host_esp
{

std::vector<LogLine> log_lines()
{
    std::lock_guard<std::mutex> guard(s_log_lock);
    return s_log;
}

void clear_log()
{
    std::lock_guard<std::mutex> guard(s_log_lock);
    s_log.clear();
}

std::string log_signature()
{
    std::string out;
    for (const LogLine &line : log_lines()) {
        if (line.level > ESP_LOG_INFO || line.level == ESP_LOG_NONE) {
            continue;
        }
        std::string text;
        // Digits are dropped from the places where they are a time or a
        // speed; what the numbers mean in the other lines stays.
        for (char c : line.text) {
            text += c;
        }
        out += std::to_string(line.level);
        out += " ";
        out += line.tag;
        out += ": ";
        out += text;
        out += "\n";
    }
    return out;
}

void reset_sd_card()
{
    std::error_code ignored;
    std::filesystem::remove_all(HOST_SD_ROOT, ignored);
    std::filesystem::create_directories(HOST_SD_ROOT);
}

}  // namespace host_esp
