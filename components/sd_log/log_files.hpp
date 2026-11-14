#pragma once

// Naming, pruning and line handling of the log files on the SD card
// ("log-00042.txt"). No ESP-IDF dependencies, so they can be unit-tested on
// the host.

#include <cstddef>
#include <string>
#include <vector>

namespace sdlog
{

// Number of a log file name ("log-00042.txt" -> 42), or 0 if the name is not
// one of ours. Number 0 is not used for files.
unsigned file_number(const char *name);

// Name of log file `number`: "log-" + at least five digits + ".txt".
std::string file_name(unsigned number);

// Of the log files with these numbers, the ones to remove so that at most
// `keep` remain: the lowest (oldest) numbers, in ascending order.
std::vector<unsigned> files_to_remove(std::vector<unsigned> numbers,
                                      unsigned keep);

// Length of a formatted log line to store, for vsnprintf's result `length`
// in a buffer of `capacity` bytes. A line that did not fit is cut to the
// buffer and ends with '\n' again (written to `line`). 0 for an empty line
// or an error (a negative `length`).
std::size_t fit_line(char *line, int length, std::size_t capacity);

// Writes the mark of a cycle's log lines ("#12 ") to `line` and returns its
// length; 0 (nothing written) for cycle 0 (no cycle runs) or a buffer
// without room for it.
std::size_t cycle_prefix(char *line, std::size_t capacity, unsigned cycle);

}  // namespace sdlog
