#pragma once

// Diagnostic commands of the serial console (CONFIG_HUB_CONSOLE_DIAGNOSTICS):
// the internal RAM, and the files on the SD card. Paths are below the card's
// root: "/sdcard/a", "/a" and "a" are the same; ".." is rejected.

#include <string>

namespace hub_console
{

// Writes "TCH <kind> <message>" (hub_console.cpp).
void reply(const char *kind, const std::string &message);

// Internal RAM by capability and by region; with `blocks` the blocks of the
// small DMA-capable heaps, with the first bytes of each used one.
void run_heap(bool blocks);

// One "TCH INFO <d|f> <size> <mtime> <name>" line per entry.
void run_ls(const std::string &path);

// The file's bytes as "TCH DATA <offset> <base64>" lines between
// "TCH FILE <size>" and "TCH CRC <offset> <length> <crc32>".
void run_cat(const std::string &path, long long offset, long long length);

// Deletes a file or, with `recursive`, a directory tree. Refused while
// `busy` (the camera session writes or the backend sync reads the card).
void run_rm(const std::string &path, bool recursive, bool busy);

// Writes a test file in chunks and reports the card's write rate.
void run_sdbench(long long total_kb, long long chunk_kb, bool busy);

}  // namespace hub_console
