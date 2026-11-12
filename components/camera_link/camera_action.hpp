#pragma once

// The actions the hub can ask a camera to do (#135, #194), described once for
// every front end: the serial console, the web UI and Telegram all turn their
// input into an Action and get the same Outcome back. The camera session sends
// the request and judges the answer with this module. Pure logic (no ESP-IDF),
// host-tested.
//
// What the camera does is in docs/camera-media-delete.md and
// docs/udp-protocol.md. Two rules come from there:
//   - `result` is 0 even when nothing happened. Success is the command's own
//     field (snapRet, recordRet, delRet, formatRet), never `result` alone.
//   - dir/num identify a file only until the camera reuses them, so a
//     deletion is addressed by mediaId and looked up in a fresh media list.

#include <cstdint>
#include <optional>
#include <string>

#include "device_info.hpp"
#include "media_catalog.hpp"

namespace camera::action
{

enum class Type : std::uint8_t {
    photo,         // 641 TRIGGER_SNAP
    video_start,   // 643 TRIGGER_RECORD
    video_stop,    // 644 STOP_RECORD
    delete_media,  // 773 DELETE_MEDIA, one file by mediaId
    format_sd,     // 518 FORMAT_SD_CARD, the whole card
};

// "photo", "video-start", "video-stop", "delete" and "format": the names the
// console and the queue file use.
const char *type_name(Type type);
std::optional<Type> parse_type(const std::string &name);

// The camera command (cmdId) of an action, and the action of a cmdId from an
// answer (nullopt for any other command).
std::uint32_t cmd_id(Type type);
std::optional<Type> type_of_cmd(std::int64_t cmd_id);

// Who asks. The same action may be allowed from one front end and not from
// another.
enum class Source : std::uint8_t {
    console,   // the serial console: someone is at the hub
    web_ui,    // the hub's web page (password protected)
    telegram,  // a chat, over the internet
    backend,   // a command from the backend
};

const char *source_name(Source source);

// Photo and video need only the chat's trust. Deleting needs someone at the
// hub or its password-protected page; formatting the card could lose
// recordings the hub has not fetched, so it is never done remotely (#196).
bool allowed(Type type, Source source);

// The action cannot be undone and the front end must ask for a confirmation
// (see ConfirmGate) before it queues it.
bool needs_confirmation(Type type);

// Deleting needs a fresh media list in the session, to find the file by its
// mediaId right before the command; the other actions do not.
bool needs_media_list(Type type);

struct Action {
    Type type = Type::photo;
    // delete only: the recording's mediaId (never dir/num, which are reused).
    std::int64_t media_id = 0;
};

// Empty if the action is complete, else why not (e.g. a delete without a
// mediaId).
std::string validate(const Action &action);

// Empty if the camera can do the action in the state its device info (512)
// reports, else why not (e.g. a photo with a full card).
std::string check_camera(Type type, const artemis::DeviceInfo &info);

// Where a file is on the camera right now, taken from the fresh media list.
struct Target {
    std::int64_t file_type = 0;
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
};

// The file with this mediaId in the freshly read list of the session (a
// `listed` entry); nullopt if the camera does not list it any more, then the
// file is gone and the action must not be sent.
std::optional<Target> find_target(const artemis::MediaCatalog &catalog,
                                  std::int64_t media_id);

// The fields that follow "cmdId" in the request, e.g.
// ",\"fileType\":2,\"dirNum\":100,\"mediaNum\":1" for a deletion (note
// dirNum, not mediaDirNum); empty for the actions without parameters. A
// deletion needs `target`.
std::string request_fields(const Action &action,
                           const std::optional<Target> &target);

struct Outcome {
    bool done = false;
    // The command's own result field; 0 when done, else the camera's code.
    std::int64_t code = 0;
    // Why it failed, in words for a log line or a chat message; empty when
    // done.
    std::string reason;
};

// Judges the camera's answer to an action. Success is the command's own
// field: snapRet (641), recordRet (643), delRet (773), formatRet (518).
// 644 has none (`{"errorMsg":"Success","result":0,"cmdId":644}`), so there
// `result` 0 counts. An answer without the field is a failure.
Outcome interpret(Type type, const std::string &json);

// The failure for an action the camera never answered or that could not be
// sent.
Outcome failure(const std::string &reason);

}  // namespace camera::action
