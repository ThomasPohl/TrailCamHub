#pragma once

// The hub's action queue as a service (#194): one queue for every front end
// and the camera session, kept on the SD card. The front ends submit actions
// and read the results; the camera session takes the next action, sends it and
// reports the camera's answer. All functions may be called from any task.
//
// The rules (what is allowed from where, the confirmation, the order) are in
// the pure modules camera_action.hpp, action_confirm.hpp and action_queue.hpp.

#include <string>
#include <vector>

#include "action_confirm.hpp"
#include "action_queue.hpp"

namespace camera::action::service
{

// Loads the queue from the SD card (an action that was running when the hub
// restarted fails, see action_store.hpp). Call once at boot after the SD card
// is mounted; before that, and without a card, the queue is empty and not
// saved.
void init();

// Queues an action. `key` recognises a command that arrives twice (e.g.
// "tg:<update id>"); empty for none. Does not check the confirmation: the
// front end does that for the actions that need it (needs_confirmation()).
ActionQueue::AddResult submit(const Action &action, Source source,
                              const std::string &key);

// The confirmation of an action that cannot be undone: request_confirmation()
// returns the code to repeat, confirm() checks it. `subject` names the
// action, e.g. "format" or "delete 123".
std::string request_confirmation(const std::string &subject);
ConfirmGate::Result confirm(const std::string &subject,
                            const std::string &code);

// A copy of the queue, oldest first, for the front ends.
std::vector<Entry> snapshot();

// Finished entries of `source` that were not reported yet; they are marked
// as reported.
std::vector<Entry> take_unreported(Source source);

bool has_pending();
bool pending_needs_list();

// For the camera session.
struct Job {
    std::uint32_t id = 0;
    Action action;
};
// The oldest waiting action the session can run now (`list_ready`: the media
// list of this session is read, so a deletion may run); it becomes running.
bool take_next(bool list_ready, Job &job);
void finish(std::uint32_t id, const Outcome &outcome);
// The session ended: whatever was sent and not answered ends as failed.
void abandon_running(const std::string &reason);
// The camera cannot do what waits (e.g. it has no SD card).
void fail_pending(const std::string &reason);

}  // namespace camera::action::service
