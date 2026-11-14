#pragma once

// Parses the line-based serial commands of the hub, as sent by the web
// installer or typed into a terminal:
//
//   help | status | scan | camlist | upload | update | stop
//   photo | video start | video stop | actions
//   delete <mediaId> [confirm <code>]
//   format [confirm <code>]
//   config show | config clear
//   config set <key> <value...>   value = rest of the line (empty erases)
//   config set-hex <key> <hex>    value as hex-encoded bytes (any characters)
//   config unset <key>
//
// and the diagnostic commands for the SD card and the memory:
//
//   heap [blocks]
//   ls [path...]                  path = rest of the line
//   cat <path> [offset [length]]
//   rm [-r] <path...>             path = rest of the line
//   sdbench [total_kb [chunk_kb]]
//
// A leading "tch " is accepted and ignored. Pure logic, host-testable.

#include <string>

namespace hub_console
{

enum class CommandType {
    empty,
    help,
    status,
    scan,
    camlist,
    upload,
    update,
    stop,
    photo,
    video_start,
    video_stop,
    delete_media,
    format_sd,
    actions,  // list the queued camera actions and their results
    restart,
    ping,  // only keeps the hub awake (the installer sends it while connected)
    heap,
    ls,
    cat,
    rm,
    sdbench,
    config_show,
    config_clear,
    config_set,
    invalid,
};

struct Command {
    CommandType type = CommandType::empty;
    std::string key;         // config_set
    std::string value;       // config_set (empty = erase)
    std::string error;       // invalid
    std::string path;        // ls (may be empty), cat, rm
    bool recursive = false;  // rm -r
    bool blocks = false;     // heap blocks
    // cat: start and number of bytes (-1: to the end); sdbench: sizes in KB.
    long long offset = 0;
    long long length = -1;
    long long total_kb = 4096;
    long long chunk_kb = 8;
    // delete: the recording's mediaId; delete and format: the second step,
    // with the code the first one printed.
    long long media_id = 0;
    bool confirm = false;
    std::string code;
};

Command parse_command(const std::string &line);

// Decodes an even-length hex string; returns false on invalid input.
bool hex_decode(const std::string &hex, std::string &out);

// Text for the help command, one command per line.
const char *help_text();

}  // namespace hub_console
