#pragma once

// The one way a front end asks for a camera action (#135, #194): the serial
// console and the web UI both call request(), which checks what the action
// may do, asks for the confirmation where it cannot be undone, queues it in
// the action queue and starts the session that runs it. The rules themselves
// are in camera_action.hpp, action_confirm.hpp and action_queue.hpp; the
// answer is the same for both, so each only words it for its medium
// (console line, JSON). Telegram queues directly: its read is scheduled by
// the check-in, and it never needs a confirmation.

#include <cstddef>
#include <string>

namespace session_actions
{

enum class From {
    console,
    web_ui,
};

struct Reply {
    // Queued (or already queued) or, for the first step of a confirmation,
    // waiting for the code.
    bool ok = false;
    // The first step of delete or format: repeat the request with `code`.
    bool needs_code = false;
    std::string code;
    // The queue entry, when queued.
    unsigned id = 0;
    // A session was started that runs it now; false: it waits for the next
    // camera session (one is running).
    bool session_started = false;
    // In words, for people.
    std::string text;
};

// `type` is "photo", "video-start", "video-stop", "delete" or "format";
// `media_id` only for a delete. `code` is empty for the first step of an
// action that needs a confirmation and the code from the first step for the
// second one. Never blocks.
Reply request(From from, const std::string &type, long long media_id,
              const std::string &code);

// `{"ok":true,"id":3,"started":true,"text":"…"}`, or with "needsCode":true and
// "code":"4821". Valid JSON for any text.
std::string to_json(const Reply &reply);

// `{"actions":[{"id":3,"text":"photo: done"},…]}` with the newest `limit`
// entries of the queue, oldest first.
std::string queue_json(std::size_t limit);

}  // namespace session_actions
