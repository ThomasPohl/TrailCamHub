/*
 * Diagnostic commands of the serial console (see diagnostics.hpp): the
 * internal RAM, and listing, reading, deleting and benchmarking files on the
 * SD card.
 */

#include "diagnostics.hpp"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "sd_card.h"
#include "sd_log.h"

// Raw bytes per "TCH DATA" line: 192 bytes are 256 Base64 characters.
#define CAT_LINE_BYTES 192
#define CAT_READ (CAT_LINE_BYTES * 32)

namespace hub_console
{
namespace
{

const char *TAG = "trailcamhub_console";

// Turns a path argument into a path below the SD card root: "" for the root,
// "a/b" otherwise. Rejects ".." segments; "." and empty segments are dropped.
bool relative_path(const std::string &in, std::string &out)
{
    std::string path = in;
    const std::string mount = SD_CARD_MOUNT_POINT;
    if (path.compare(0, mount.size(), mount) == 0 &&
        (path.size() == mount.size() || path[mount.size()] == '/')) {
        path.erase(0, mount.size());
    }
    out.clear();
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string segment = path.substr(start, end - start);
        if (segment == "..") {
            return false;
        }
        if (!segment.empty() && segment != ".") {
            if (!out.empty()) {
                out += '/';
            }
            out += segment;
        }
        start = end + 1;
    }
    return true;
}

std::string absolute(const std::string &relative)
{
    return relative.empty() ? std::string(SD_CARD_MOUNT_POINT)
                            : std::string(SD_CARD_MOUNT_POINT) + "/" + relative;
}

bool ends_with(const std::string &text, const char *suffix)
{
    const size_t length = strlen(suffix);
    return text.size() >= length &&
           text.compare(text.size() - length, length, suffix) == 0;
}

// Files that are open for writing, which must not be removed because that
// corrupts the FAT volume: the log being written and unfinished downloads
// (".part") or catalog updates (".tmp").
bool file_in_use(const std::string &path)
{
    if (ends_with(path, ".part") || ends_with(path, ".tmp")) {
        return true;
    }
#if CONFIG_SD_LOG_ENABLE
    char log_path[64];
    sd_log_current_path(log_path, sizeof(log_path));
    if (path == log_path) {
        return true;
    }
#endif
    return false;
}

std::string format(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
std::string format(const char *format, ...)
{
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// Free, largest free block and minimum free size for heaps with `caps`.
void print_heap(const char *name, uint32_t caps)
{
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    reply("INFO", format("%-14s %7u free, %7u largest, %7u minimum, %4u blocks",
                         name, (unsigned)info.total_free_bytes,
                         (unsigned)info.largest_free_block,
                         (unsigned)info.minimum_free_bytes,
                         (unsigned)info.allocated_blocks));
}

// A block of a heap, copied while the heap is walked.
struct BlockCopy {
    uintptr_t address;
    uint32_t size;
    bool used;
    uint8_t head[16];
};

struct BlockList {
    BlockCopy *blocks;
    size_t capacity;
    size_t count;
};

// Copies the blocks of the small DMA-capable heaps (the pool reserved by
// CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL among them); the large main heap is
// skipped. Runs with the heap locked, so it must not print or allocate.
bool copy_block(walker_heap_into_t heap, walker_block_info_t block, void *arg)
{
    BlockList *list = static_cast<BlockList *>(arg);
    if (heap.end - heap.start > 64 * 1024 || list->count >= list->capacity) {
        return false;
    }
    BlockCopy &copy = list->blocks[list->count++];
    copy.address = reinterpret_cast<uintptr_t>(block.ptr);
    copy.size = block.size;
    copy.used = block.used;
    if (block.used) {
        memcpy(copy.head, block.ptr, block.size < 16 ? block.size : 16);
    }
    return true;
}

// Lists the copied blocks, with the first bytes of each used block as a hint
// to its owner.
void print_blocks()
{
    BlockList list = {};
    list.capacity = 512;
    list.blocks = static_cast<BlockCopy *>(
        heap_caps_malloc(list.capacity * sizeof(BlockCopy),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (list.blocks == NULL) {
        list.blocks =
            static_cast<BlockCopy *>(malloc(list.capacity * sizeof(BlockCopy)));
    }
    if (list.blocks == NULL) {
        reply("ERR", "no memory");
        return;
    }
    heap_caps_walk(MALLOC_CAP_DMA, copy_block, &list);
    reply("INFO", "blocks of the DMA-capable heaps below 64 KB:");
    for (size_t i = 0; i < list.count; i++) {
        const BlockCopy &block = list.blocks[i];
        std::string line =
            format("%08x %6u %s", (unsigned)block.address, (unsigned)block.size,
                   block.used ? "used" : "free");
        if (block.used) {
            const size_t count = block.size < 16 ? block.size : 16;
            line += " ";
            for (size_t j = 0; j < count; j++) {
                line += format(" %02x", block.head[j]);
            }
        }
        reply("INFO", line);
    }
    free(list.blocks);
}

// Removes the tree below `path`, then `path` itself. Files in use are kept
// (and with them their directories) and counted in `kept`.
bool remove_tree(const std::string &path, unsigned &removed, unsigned &kept)
{
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    if (S_ISDIR(info.st_mode)) {
        // Collected first, so the directory is not changed while it is read.
        std::vector<std::string> names;
        DIR *dir = opendir(path.c_str());
        if (dir == NULL) {
            return false;
        }
        while (struct dirent *entry = readdir(dir)) {
            names.push_back(entry->d_name);
        }
        closedir(dir);
        bool ok = true;
        for (const std::string &name : names) {
            ok = remove_tree(path + "/" + name, removed, kept) && ok;
        }
        if (!ok) {
            return false;
        }
        if (rmdir(path.c_str()) != 0) {
            ESP_LOGW(TAG, "Could not remove %s: errno %d", path.c_str(), errno);
            return false;
        }
        removed++;
        return true;
    }
    if (file_in_use(path)) {
        reply("INFO", "kept " + path + " (being written)");
        kept++;
        return false;
    }
    if (remove(path.c_str()) != 0) {
        ESP_LOGW(TAG, "Could not delete %s: errno %d", path.c_str(), errno);
        return false;
    }
    removed++;
    return true;
}

}  // namespace

// Internal RAM by capability and by region, to see which memory a component
// is short of (the BLE controller, for example, needs DMA-capable RAM).
void run_heap(bool blocks)
{
    if (blocks) {
        print_blocks();
        return;
    }
    print_heap("internal", MALLOC_CAP_INTERNAL);
    print_heap("internal 8bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap("DMA", MALLOC_CAP_DMA);
    print_heap("RTC RAM", MALLOC_CAP_RTCRAM);
    print_heap("retention", MALLOC_CAP_RETENTION);
    print_heap("PSRAM", MALLOC_CAP_SPIRAM);
    // Plain lines, one region after the other.
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
}

void run_ls(const std::string &path)
{
    std::string relative;
    if (!relative_path(path, relative)) {
        reply("ERR", "bad path");
        return;
    }
    const std::string directory = absolute(relative);
    DIR *dir = opendir(directory.c_str());
    if (dir == NULL) {
        reply("ERR", "no directory");
        return;
    }
    while (struct dirent *entry = readdir(dir)) {
        const std::string full = directory + "/" + entry->d_name;
        struct stat info = {};
        stat(full.c_str(), &info);
        reply("INFO",
              format("%c %lld %lld ", S_ISDIR(info.st_mode) ? 'd' : 'f',
                     (long long)info.st_size, (long long)info.st_mtime) +
                  entry->d_name);
    }
    closedir(dir);
}

// The CRC-32 (as zlib computes it) covers the bytes sent. The USB-Serial-JTAG
// driver drops output that the host does not read in time, so the offsets let
// the reader find gaps and request them again.
void run_cat(const std::string &path_argument, long long offset,
             long long length)
{
    std::string relative;
    if (!relative_path(path_argument, relative) || relative.empty()) {
        reply("ERR", "bad path");
        return;
    }
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || S_ISDIR(info.st_mode)) {
        reply("ERR", "no file");
        return;
    }
    if (offset > (long long)info.st_size) {
        reply("ERR", "bad offset");
        return;
    }
    if (length < 0 || offset + length > (long long)info.st_size) {
        length = info.st_size - offset;
    }
    FILE *file = fopen(path.c_str(), "rb");
    if (file == NULL) {
        reply("ERR", "cannot open");
        return;
    }
    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        reply("ERR", "cannot seek");
        return;
    }
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(CAT_READ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == NULL) {
        buffer = static_cast<uint8_t *>(malloc(CAT_READ));
    }
    if (buffer == NULL) {
        fclose(file);
        reply("ERR", "no memory");
        return;
    }
    reply("FILE", std::to_string((long long)info.st_size));
    unsigned char text[CAT_LINE_BYTES * 4 / 3 + 4];
    uint32_t crc = 0;
    long long position = offset;
    long long left = length;
    bool read_error = false;
    while (left > 0) {
        const size_t want = left > CAT_READ ? CAT_READ : (size_t)left;
        const size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            read_error = true;
            break;
        }
        crc = esp_rom_crc32_le(crc, buffer, got);
        for (size_t done = 0; done < got; done += CAT_LINE_BYTES) {
            const size_t line =
                got - done > CAT_LINE_BYTES ? CAT_LINE_BYTES : got - done;
            size_t text_length = 0;
            mbedtls_base64_encode(text, sizeof(text), &text_length,
                                  buffer + done, line);
            printf("TCH DATA %lld %.*s\n", position + (long long)done,
                   (int)text_length, text);
        }
        position += got;
        left -= got;
    }
    free(buffer);
    fclose(file);
    reply("CRC", format("%lld %lld %08lx", offset, position - offset,
                        (unsigned long)crc));
    if (read_error) {
        reply("ERR", "read error");
    }
}

void run_rm(const std::string &path_argument, bool recursive, bool busy)
{
    std::string relative;
    if (!relative_path(path_argument, relative)) {
        reply("ERR", "bad path");
        return;
    }
    if (relative.empty()) {
        reply("ERR", "the root cannot be removed");
        return;
    }
    if (busy) {
        reply("ERR", "a scan is running; stop it first");
        return;
    }
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        reply("ERR", "no such file");
        return;
    }
    if (S_ISDIR(info.st_mode) && !recursive) {
        reply("ERR", "is a directory; use rm -r");
        return;
    }
    unsigned removed = 0;
    unsigned kept = 0;
    const bool ok = remove_tree(path, removed, kept);
    ESP_LOGI(TAG, "Removed %s (%u entries)", path.c_str(), removed);
    if (kept != 0) {
        reply("ERR",
              format("removed %u entries; files in use were kept", removed));
    } else if (!ok) {
        reply("ERR", format("removed %u entries; not everything could be "
                            "removed",
                            removed));
    } else {
        reply("OK", format("removed %u entries", removed));
    }
}

// Writes a test file in chunks from internal DMA-capable RAM, unbuffered like
// the SD writer of the video download, and reports the throughput and the
// slowest chunk; the file is removed afterwards.
void run_sdbench(long long total_kb, long long chunk_kb, bool busy)
{
    if (busy) {
        reply("ERR", "a scan is running; stop it first");
        return;
    }
    const size_t chunk = (size_t)chunk_kb * 1024;
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (buffer == NULL) {
        reply("ERR", "no internal DMA-capable memory for the chunk");
        return;
    }
    for (size_t i = 0; i < chunk; i++) {
        buffer[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    const std::string path = absolute("sdbench.tmp");
    FILE *file = fopen(path.c_str(), "wb");
    if (file == NULL) {
        free(buffer);
        reply("ERR", "cannot create the test file");
        return;
    }
    setvbuf(file, NULL, _IONBF, 0);
    const long long chunks = (total_kb + chunk_kb - 1) / chunk_kb;
    int64_t slowest_us = 0;
    const int64_t started_us = esp_timer_get_time();
    bool ok = true;
    for (long long i = 0; i < chunks && ok; i++) {
        const int64_t chunk_started_us = esp_timer_get_time();
        ok = fwrite(buffer, 1, chunk, file) == chunk;
        const int64_t took_us = esp_timer_get_time() - chunk_started_us;
        if (took_us > slowest_us) {
            slowest_us = took_us;
        }
    }
    const int64_t written_us = esp_timer_get_time() - started_us;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    const int64_t total_us = esp_timer_get_time() - started_us;
    remove(path.c_str());
    free(buffer);
    if (!ok) {
        reply("ERR", "write failed");
        return;
    }
    const long long bytes = chunks * (long long)chunk;
    reply("OK",
          format("sdbench: %lld KB in %lld chunks of %lld KB: %lld KB/s "
                 "(writes %lld ms, with sync %lld ms), slowest chunk "
                 "%lld ms",
                 bytes / 1024, chunks, chunk_kb,
                 (long long)(bytes * 1000 / 1024 / (total_us / 1000 + 1)),
                 (long long)(written_us / 1000), (long long)(total_us / 1000),
                 (long long)(slowest_us / 1000)));
}

}  // namespace hub_console
