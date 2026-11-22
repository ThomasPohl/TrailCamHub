#pragma once

// What the hub answers to a "deleteMedia" (#260) or "setSettings" (#368)
// command from the backend, from what the action queue says. Pure logic,
// host-tested; uplink.cpp queues the action and sends the answer.
//
// The command is answered only when it is settled. Until then the backend
// sends it again with every check-in, and the queue recognises it by its key
// ("be:<command id>"), so it is queued once. A deletion runs in the next
// camera session, which the hub starts right away; the answer goes back with
// the check-in after it, usually in the same wake cycle.

#include <string>

#include "action_queue.hpp"

namespace uplink
{

struct DeleteDecision {
    // Acknowledge the command now (with `done` / `error`); otherwise it
    // comes again.
    bool answer = false;
    bool done = false;
    std::string error;
    // A camera session is needed to run it.
    bool read_needed = false;
};

// The queue key of a backend command.
std::string delete_key(const std::string &command_id);

// `added`: what submitting the deletion returned. `entry`: for a duplicate,
// the queued entry it matched (null if the queue no longer has it).
DeleteDecision
decide_delete(const camera::action::ActionQueue::AddResult &added,
              const camera::action::Entry *entry);

// The same for a "setSettings" command (#368): queued once by its key, run
// in the camera's next session, answered with the queue's result. Unlike a
// deletion, every failure is a failure.
DeleteDecision
decide_settings(const camera::action::ActionQueue::AddResult &added,
                const camera::action::Entry *entry);

}  // namespace uplink
