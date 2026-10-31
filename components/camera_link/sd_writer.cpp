#include "sd_writer.hpp"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace camera::store
{
namespace
{

const char *TAG = "trailcamhub_sd_writer";

// About 10 s of video at 400 KB/s. With 1 MB the session still waited about
// 6 s per 15-second video for the card, and frames it did not receive
// meanwhile were sent again by the camera.
constexpr std::size_t kBufferSize = 4 * 1024 * 1024;
// Largest piece queued at a time.
constexpr std::size_t kItemSize = 8 * 1024;
// The data of each file is collected in a buffer in internal (DMA-capable)
// RAM and written when the buffer is full, so that the card gets large,
// sector-aligned writes instead of one write per video or audio frame (the
// SD driver waits for the card actively, so every write costs CPU time). In a
// benchmark the card took 16 KB pieces about 13 % faster than 8 KB; smaller
// buffers are used if internal RAM is short.
constexpr std::size_t kLargeSlotSizes[] = {16 * 1024, 8 * 1024, 4 * 1024};
// A video is written to two files at once: the H.264 file gets the large
// buffer, the AAC file (about 130 KB per video, in pieces of about 300 bytes)
// a small one, to spare internal RAM during the session.
constexpr std::size_t kSmallSlotSize = 4 * 1024;
constexpr std::size_t kSlotCount = 2;
// A file whose first piece is smaller goes to the small buffer.
constexpr std::size_t kSmallPiece = 2 * 1024;
constexpr std::size_t kSectorSize = 512;
constexpr std::uint32_t kStackSize = 4096;
// Below the camera session (5), so that receiving always comes first.
constexpr UBaseType_t kPriority = 3;

// A piece of data for `file`; with file null a flush marker, or with `end`
// set the end of the files `ends`.
struct Item {
    MediaFileWriter *file;
    MediaFileWriter *ends[2];
    std::uint32_t end;
};

// Data collected for one file, not yet written.
struct Slot {
    MediaFileWriter *file;
    std::uint8_t *buffer;
    std::size_t size;
    std::size_t used;
    // The file's partial last sector has been filled, so that the buffer
    // starts at a sector boundary.
    bool aligned;
};

RingbufHandle_t s_ring;
// The large buffer first, then the small one.
Slot s_slots[kSlotCount];
SemaphoreHandle_t s_flushed;
// Number of the last end marker the task reached, and whether the writes
// before it succeeded.
std::atomic<std::uint32_t> s_ended{0};
std::atomic<bool> s_ended_ok{true};
std::uint32_t s_ends_queued;
volatile bool s_discard;
volatile bool s_failed;
bool s_started;
bool s_unavailable;
portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
std::int64_t s_busy_us;
std::int64_t s_longest_us;

// Writes to the card and adds the time to the statistics.
bool write_file(MediaFileWriter *file, const std::uint8_t *data,
                std::size_t size)
{
    const std::int64_t started_us = esp_timer_get_time();
    const bool ok = file->write(data, size);
    const std::int64_t took_us = esp_timer_get_time() - started_us;
    portENTER_CRITICAL(&s_stats_lock);
    s_busy_us += took_us;
    if (took_us > s_longest_us) {
        s_longest_us = took_us;
    }
    portEXIT_CRITICAL(&s_stats_lock);
    return ok;
}

// Writes what is collected for the slot's file and frees the slot.
bool flush_slot(Slot &slot, bool write)
{
    const bool ok = !write || slot.used == 0 ||
                    write_file(slot.file, slot.buffer, slot.used);
    slot.file = nullptr;
    slot.used = 0;
    slot.aligned = false;
    return ok;
}

// The slot of `file`, or a free one: the large one for a file that starts
// with a large piece, the small one otherwise. If neither is free, the
// preferred slot's file is written out (its next data starts a new run).
Slot &slot_for(MediaFileWriter *file, std::size_t first_piece, bool &ok)
{
    for (Slot &slot : s_slots) {
        if (slot.file == file) {
            return slot;
        }
    }
    const std::size_t preferred = first_piece < kSmallPiece ? 1 : 0;
    for (const std::size_t index : {preferred, 1 - preferred}) {
        if (s_slots[index].file == nullptr) {
            s_slots[index].file = file;
            return s_slots[index];
        }
    }
    ok = flush_slot(s_slots[preferred], true) && ok;
    s_slots[preferred].file = file;
    return s_slots[preferred];
}

// Collects data for a file and writes every full buffer.
bool collect(MediaFileWriter *file, const std::uint8_t *data, std::size_t size)
{
    bool ok = true;
    Slot &slot = slot_for(file, size, ok);
    if (!slot.aligned) {
        // Fill the file's current sector first (FAT copies a partial sector
        // into its cache), so that the buffer is written at sector
        // boundaries, several sectors in one transfer.
        std::size_t head =
            (kSectorSize - file->size() % kSectorSize) % kSectorSize;
        if (head > size) {
            head = size;
        }
        ok = (head == 0 || write_file(file, data, head)) && ok;
        data += head;
        size -= head;
        slot.aligned = size > 0 || file->size() % kSectorSize == 0;
    }
    while (ok && size > 0) {
        const std::size_t room = slot.size - slot.used;
        const std::size_t take = size < room ? size : room;
        memcpy(slot.buffer + slot.used, data, take);
        slot.used += take;
        data += take;
        size -= take;
        if (slot.used == slot.size) {
            ok = write_file(file, slot.buffer, slot.used);
            slot.used = 0;
        }
    }
    return ok;
}

// Started only with PSRAM (see sd_writer_start()).
[[maybe_unused]] void writer_task(void *)
{
    for (;;) {
        std::size_t size = 0;
        auto *item = static_cast<std::uint8_t *>(
            xRingbufferReceive(s_ring, &size, portMAX_DELAY));
        if (item == nullptr) {
            continue;
        }
        Item header;
        memcpy(&header, item, sizeof(header));
        if (header.end != 0) {
            // End of two files: write what is collected for them, so that
            // the caller can close them, then report the end.
            for (Slot &slot : s_slots) {
                if (slot.file != nullptr && (slot.file == header.ends[0] ||
                                             slot.file == header.ends[1])) {
                    if (!flush_slot(slot, !s_discard && !s_failed)) {
                        s_failed = true;
                    }
                }
            }
            vRingbufferReturnItem(s_ring, item);
            s_ended_ok = !s_failed;
            s_failed = false;
            s_ended = header.end;
            continue;
        }
        if (header.file == nullptr) {
            // Flush: write the rest of every file before the caller closes
            // or removes it.
            const bool write = !s_discard && !s_failed;
            for (Slot &slot : s_slots) {
                if (!flush_slot(slot, write)) {
                    s_failed = true;
                }
            }
            vRingbufferReturnItem(s_ring, item);
            s_discard = false;
            xSemaphoreGive(s_flushed);
            continue;
        }
        if (!s_discard && !s_failed &&
            !collect(header.file, item + sizeof(header),
                     size - sizeof(header))) {
            s_failed = true;
        }
        vRingbufferReturnItem(s_ring, item);
    }
}

bool send_item(const Item &header, const std::uint8_t *data, std::size_t size)
{
    void *slot = nullptr;
    if (xRingbufferSendAcquire(s_ring, &slot, sizeof(Item) + size,
                               portMAX_DELAY) != pdTRUE) {
        return false;
    }
    memcpy(slot, &header, sizeof(header));
    if (size > 0) {
        memcpy(static_cast<std::uint8_t *>(slot) + sizeof(header), data, size);
    }
    return xRingbufferSendComplete(s_ring, slot) == pdTRUE;
}

void free_slots()
{
    for (Slot &slot : s_slots) {
        heap_caps_free(slot.buffer);
        slot = {};
    }
}

std::uint8_t *allocate_internal(std::size_t size)
{
    return static_cast<std::uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

}  // namespace

bool sd_writer_start()
{
    if (s_started) {
        return true;
    }
    if (s_unavailable) {
        return false;
    }
#if CONFIG_SPIRAM
    s_ring = xRingbufferCreateWithCaps(kBufferSize, RINGBUF_TYPE_NOSPLIT,
                                       MALLOC_CAP_SPIRAM);
    s_flushed = xSemaphoreCreateBinary();
    if (s_ring != nullptr && s_flushed != nullptr &&
        xTaskCreateWithCaps(writer_task, "sd_writer", kStackSize, nullptr,
                            kPriority, nullptr, MALLOC_CAP_SPIRAM) == pdPASS) {
        s_started = true;
        ESP_LOGI(TAG, "SD writer started (%u KB buffer in PSRAM)",
                 (unsigned)(kBufferSize / 1024));
        return true;
    }
    ESP_LOGW(TAG, "Could not start the SD writer; writing directly");
    if (s_ring != nullptr) {
        vRingbufferDeleteWithCaps(s_ring);
        s_ring = nullptr;
    }
    if (s_flushed != nullptr) {
        vSemaphoreDelete(s_flushed);
        s_flushed = nullptr;
    }
#endif
    s_unavailable = true;
    return false;
}

bool sd_writer_begin_session()
{
    if (!s_started) {
        return false;
    }
    if (s_slots[0].buffer == nullptr) {
        Slot &small = s_slots[1];
        small = {};
        small.buffer = allocate_internal(kSmallSlotSize);
        small.size = kSmallSlotSize;
        Slot &large = s_slots[0];
        large = {};
        for (const std::size_t size : kLargeSlotSizes) {
            large.buffer =
                small.buffer == nullptr ? nullptr : allocate_internal(size);
            if (large.buffer != nullptr) {
                large.size = size;
                break;
            }
        }
        if (large.buffer == nullptr) {
            free_slots();
            ESP_LOGW(TAG,
                     "No internal RAM for the SD writer; writing directly");
            return false;
        }
        ESP_LOGI(TAG, "SD writer collects %u KB and %u KB per file",
                 (unsigned)(large.size / 1024), (unsigned)(small.size / 1024));
    }
    return true;
}

void sd_writer_end_session()
{
    if (s_slots[0].buffer == nullptr) {
        return;
    }
    sd_writer_flush(true);
    free_slots();
}

bool sd_writer_active()
{
    return s_started && s_slots[0].buffer != nullptr;
}

bool sd_writer_has_room(std::size_t size)
{
    if (!sd_writer_active()) {
        return false;
    }
    // Each piece of up to kItemSize bytes takes its header and the ring
    // buffer's own header (8 bytes, rounded up to 4).
    const std::size_t pieces = (size + kItemSize - 1) / kItemSize;
    return xRingbufferGetCurFreeSize(s_ring) >=
           size + pieces * (sizeof(Item) + 12);
}

bool sd_writer_write(MediaFileWriter &file, const std::uint8_t *data,
                     std::size_t size)
{
    if (!sd_writer_active()) {
        const std::int64_t started_us = esp_timer_get_time();
        const bool ok = file.write(data, size);
        const std::int64_t took_us = esp_timer_get_time() - started_us;
        s_busy_us += took_us;
        if (took_us > s_longest_us) {
            s_longest_us = took_us;
        }
        return ok;
    }
    if (s_failed) {
        return false;
    }
    while (size > 0) {
        const std::size_t chunk = size < kItemSize ? size : kItemSize;
        if (!send_item({&file, {nullptr, nullptr}, 0}, data, chunk)) {
            return false;
        }
        data += chunk;
        size -= chunk;
    }
    return true;
}

bool sd_writer_flush(bool discard)
{
    if (s_started) {
        if (discard) {
            s_discard = true;
        }
        send_item({nullptr, {nullptr, nullptr}, 0}, nullptr, 0);
        xSemaphoreTake(s_flushed, portMAX_DELAY);
    }
    const bool ok = !s_failed;
    s_failed = false;
    return ok;
}

std::uint32_t sd_writer_end_files(MediaFileWriter *a, MediaFileWriter *b)
{
    std::uint32_t end = ++s_ends_queued;
    if (end == 0) {
        end = ++s_ends_queued;  // 0 marks data items
    }
    if (!sd_writer_active()) {
        // Written directly: everything is on the card already.
        s_ended_ok = !s_failed;
        s_failed = false;
        s_ended = end;
        return end;
    }
    send_item({nullptr, {a, b}, end}, nullptr, 0);
    return end;
}

bool sd_writer_ended(std::uint32_t end, bool &ok)
{
    if (s_ended.load() != end) {
        return false;
    }
    ok = s_ended_ok.load();
    return true;
}

void sd_writer_take_stats(std::int64_t &busy_us, std::int64_t &longest_us)
{
    portENTER_CRITICAL(&s_stats_lock);
    busy_us = s_busy_us;
    longest_us = s_longest_us;
    s_busy_us = 0;
    s_longest_us = 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

}  // namespace camera::store
