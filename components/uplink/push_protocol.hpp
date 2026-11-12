#pragma once

// Pure parts of the destinations WebDAV and Telegram (docs/modes.md): file
// names and URLs on the WebDAV share, and the bits of the Telegram Bot API
// the hub uses (updates, buttons, multipart bodies). Host-testable.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "action_queue.hpp"

namespace uplink
{
namespace push
{

// "2026-11-08_15-04-05_1176197582.jpg" from the camera's wall clock (read
// as UTC, see docs/clock.md); without a time "1176197582.jpg".
std::string file_name(std::int64_t media_time, std::int64_t media_id,
                      const char *extension);

// "2026-11-08 15:04" from the camera's wall clock; empty without a time.
std::string wall_time(std::int64_t media_time);

// Percent-encodes one path segment (RFC 3986 unreserved characters stay).
std::string encode_segment(const std::string &segment);

// `base` + "/" + the encoded segments, with exactly one slash between them.
std::string join_url(const std::string &base,
                     const std::vector<std::string> &segments);

// --- Telegram ---

struct Update {
    std::int64_t update_id = 0;
    std::int64_t chat_id = 0;  // of the message, or of the button's message
    std::string text;          // a message's text (commands start with /)
    std::string callback_id;   // set for a button press
    std::string callback_data;
};

// The updates of a getUpdates answer; false if it is not {"ok":true,...}.
bool parse_updates(const std::string &json, std::vector<Update> &out);

// "ok":true in a Bot API answer.
bool api_ok(const std::string &json);

// The command of a message ("/scan@MyBot now" -> "/scan"); empty if none.
std::string command_of(const std::string &text);

// The camera action a chat command asks for: /photo, /video_start and
// /video_stop. nullopt for any other command. Deleting and formatting are
// never asked for in a chat (camera::action::allowed()).
std::optional<camera::action::Type>
action_of_command(const std::string &command);

// What the chat is told once an action is finished, e.g. "Photo taken." or
// "Photo failed: the camera answered snapRet -2."
std::string action_result_text(const camera::action::Entry &entry);

// Button data for "get the video" (at most 64 bytes, as Telegram allows).
std::string video_button(const std::string &camera_id, std::int64_t media_id);
bool parse_video_button(const std::string &data, std::string &camera_id,
                        std::int64_t &media_id);

// reply_markup with one button.
std::string inline_keyboard(const std::string &text, const std::string &data);

// A multipart/form-data body: fields, then one file. The file's bytes go
// between head and tail.
std::string
multipart_head(const std::string &boundary,
               const std::vector<std::pair<std::string, std::string>> &fields,
               const std::string &file_field, const std::string &file_name,
               const std::string &content_type);
std::string multipart_tail(const std::string &boundary);

// The value of `key` at the top level of a JSON object, as raw JSON text
// (a string keeps its quotes); empty if missing.
std::string json_value(const std::string &object, const std::string &key);

// A raw JSON string value without its quotes and escapes (only \" \\ \/ \n
// and \uXXXX below 0x80 are decoded; others are kept as they are).
std::string json_unquote(const std::string &raw);

}  // namespace push
}  // namespace uplink
