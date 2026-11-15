#include "delete_request.hpp"

namespace uplink
{

namespace
{

// The session's reason when the fresh media list no longer has the file
// (camera_protocol.cpp). For the person who asked, it is gone: done.
constexpr const char *kNotListed = "does not list this file";

DeleteDecision answer(bool done, const std::string &error)
{
    DeleteDecision d;
    d.answer = true;
    d.done = done;
    d.error = done ? "" : error;
    return d;
}

DeleteDecision wait(bool read_needed)
{
    DeleteDecision d;
    d.read_needed = read_needed;
    return d;
}

}  // namespace

std::string delete_key(const std::string &command_id)
{
    return "be:" + command_id;
}

DeleteDecision
decide_delete(const camera::action::ActionQueue::AddResult &added,
              const camera::action::Entry *entry)
{
    using Kind = camera::action::ActionQueue::AddResult::Kind;
    using camera::action::State;
    switch (added.kind) {
    case Kind::added:
        return wait(true);
    case Kind::full:
        // The waiting ones asked for a session already; room comes with it,
        // and the command comes again.
        return wait(false);
    case Kind::invalid:
    case Kind::not_allowed:
        return answer(false, added.reason.empty()
                                 ? "the hub refused the deletion"
                                 : added.reason);
    case Kind::duplicate:
        break;
    }
    if (entry == nullptr) {
        // Dropped from the queue between the two calls; asked again with the
        // next check-in.
        return wait(false);
    }
    switch (entry->state) {
    case State::pending:
        // It asked for a session when it was queued; a camera that did not
        // answer then is tried again by the schedule, not at every check-in.
        return wait(false);
    case State::running:
        return wait(false);
    case State::done:
        return answer(true, "");
    case State::failed:
        if (entry->reason.find(kNotListed) != std::string::npos) {
            return answer(true, "");
        }
        return answer(false, entry->reason.empty() ? "the deletion failed"
                                                   : entry->reason);
    }
    return wait(false);
}

}  // namespace uplink
