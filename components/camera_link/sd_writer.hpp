#pragma once

// Writes media files on the SD card in a task of its own, so that the camera
// session never waits for the card: a write can take more than 100 ms, and
// meanwhile the camera's frames overflow the receive buffers, are lost and
// sent again only after about 400 ms. write() copies the data into a buffer
// in PSRAM and returns; flush() waits until everything is on the card. Without
// PSRAM the data is written directly.
//
// The task collects the data of each file in a buffer in internal RAM and
// writes it in large, sector-aligned pieces: the SD driver writes from PSRAM
// only one 512-byte block at a time, through a bounce buffer it allocates for
// every block, which is about half as fast and fails when internal RAM runs
// short, and it waits for the card actively, so each write costs CPU time.
// Data still collected is written by flush(), which must come before a file
// is finished or aborted.

#include <cstddef>
#include <cstdint>

#include "media_store.hpp"

namespace camera::store
{

// Starts the task and allocates its buffer in PSRAM (once, at boot). Returns
// false if that failed; writes are then made directly.
bool sd_writer_start();

// Allocates the collecting buffers in internal RAM for a camera session, and
// frees it again after the session: BLE, which runs between sessions, needs
// that RAM to find the camera. Until begin and after end, writes are made
// directly.
bool sd_writer_begin_session();
void sd_writer_end_session();

// True while writes go through the task. Files written through it need no
// stdio buffer.
bool sd_writer_active();

// True if `size` bytes can be queued without waiting for the card. False
// while writes are made directly, since those always wait.
bool sd_writer_has_room(std::size_t size);

// Queues `size` bytes for `file`. Waits only while the buffer is full. Returns
// false once a write to the card has failed (since the last flush()).
bool sd_writer_write(MediaFileWriter &file, const std::uint8_t *data,
                     std::size_t size);

// Waits until every queued write is done; with `discard`, queued data is
// dropped instead (the file is about to be removed). Returns false if a write
// failed since the last flush().
bool sd_writer_flush(bool discard = false);

// Queues the end of the files `a` and `b` (either may be null) without
// waiting: once the task has written everything queued before, including what
// it collected for them, sd_writer_ended() reports the returned number, and
// the caller may close the files. Data for other files may be queued
// meanwhile. Only the last end is reported, so wait for one before queuing
// the next.
std::uint32_t sd_writer_end_files(MediaFileWriter *a, MediaFileWriter *b);

// True once the end numbered `end` is reached; `ok` is then false if a write
// failed since the previous end or flush.
bool sd_writer_ended(std::uint32_t end, bool &ok);

// Time the task spent writing since the last call, and its longest write
// (microseconds).
void sd_writer_take_stats(std::int64_t &busy_us, std::int64_t &longest_us);

}  // namespace camera::store
