#pragma once

// What the host build of the camera session needs from "ESP-IDF": a log that
// the tests can read, a clock and the SD card as a directory.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace host_esp
{

struct LogLine {
    int level;
    std::string tag;
    std::string text;
};

// All log lines since the last clear_log(), oldest first. Lines go to stderr
// too when the environment variable HOST_SESSION_LOG is set.
std::vector<LogLine> log_lines();
void clear_log();
// The INFO and WARN lines as one text, one per line, without the numbers
// that change from run to run (times, speeds), for comparing two runs.
std::string log_signature();

// Removes and creates the directory HOST_SD_ROOT ("the SD card").
void reset_sd_card();

}  // namespace host_esp
