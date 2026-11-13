#pragma once

// The queue of actions for the cameras (#194). An action arrives from a front
// end at any time, but the camera can only be reached during a session, often
// after the hub slept; its result goes back later. So the queue is kept on the
// SD card and must survive a restart at any point:
//   - every action has an id, and an optional key from the front end (e.g. the
//     id of a Telegram update) so that a command delivered twice, after a
//     crash before the offset was saved, is queued once;
//   - an action that was running when the power failed is NOT repeated: it may
//     have been done already (a delete retried after a new recording took the
//     number would delete that one), so it ends as failed and the person asks
//     again;
//   - an action that waited too long is dropped (a photo of yesterday's
//     question is no answer).
// Pure logic, host-tested; the file is in action_store.hpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_action.hpp"

namespace camera::action
{

enum class State : std::uint8_t {
    pending,  // waiting for a camera session
    running,  // sent to the camera, no answer yet
    done,
    failed,
};

const char *state_name(State state);

struct Entry {
    std::uint32_t id = 0;
    Action action;
    Source source = Source::console;
    // From the front end, to recognise a command that arrives twice: letters,
    // digits and ":_-", at most kMaxKeyLength. May be empty.
    std::string key;
    State state = State::pending;
    // Why it failed; empty otherwise. No commas or line breaks.
    std::string reason;
    std::int64_t created_s = 0;   // Unix seconds, 0 if the clock was not set
    std::int64_t finished_s = 0;  // the same
    // The front end that asked has been told the result.
    bool notified = false;
};

class ActionQueue
{
  public:
    // Actions waiting at most; a person at the console can wait for one to
    // finish, and a chat cannot fill the card.
    static constexpr std::size_t kMaxPending = 8;
    // Entries kept, finished ones included, so that the results can still be
    // read and the keys still recognised. The oldest finished one goes first.
    static constexpr std::size_t kMaxEntries = 32;
    static constexpr std::size_t kMaxKeyLength = 40;
    static constexpr std::size_t kMaxReasonLength = 100;
    // A pending action older than this is dropped (expire_stale()).
    static constexpr std::int64_t kMaxAgeS = 24 * 3600;

    struct AddResult {
        enum class Kind {
            added,
            duplicate,    // the key or the same pending action is queued
            full,         // kMaxPending waiting already
            invalid,      // validate() said no
            not_allowed,  // this source may not ask for this action
        };
        Kind kind = Kind::invalid;
        // The new entry, or the one that makes it a duplicate.
        std::uint32_t id = 0;
        // For invalid: why.
        std::string reason;
    };

    AddResult add(const Action &action, Source source, const std::string &key,
                  std::int64_t now_s);

    // The oldest pending action, or nullptr.
    const Entry *next_pending() const;
    // pending → running. False if the id is not pending.
    bool start(std::uint32_t id);
    // running (or pending) → done or failed, with the camera's outcome.
    bool finish(std::uint32_t id, const Outcome &outcome, std::int64_t now_s);
    // Every running entry ends as failed("interrupted ..."): the hub restarted
    // or the session ended without an answer. Returns how many.
    std::size_t abandon_running(const std::string &reason, std::int64_t now_s);
    // Pending entries older than kMaxAgeS fail with "waited too long". Does
    // nothing without a valid clock (`now_s` <= 0). Returns how many.
    std::size_t expire_stale(std::int64_t now_s);
    // Every pending entry fails with `reason` (e.g. the camera has no SD
    // card). Returns how many.
    std::size_t fail_pending(const std::string &reason, std::int64_t now_s);

    const Entry *find(std::uint32_t id) const;
    std::size_t pending_count() const;
    bool has_pending_of(Type type) const;
    // True if a pending entry needs the camera's media list (a deletion).
    bool has_pending_needing_list() const;

    // Finished entries of `source` whose result was not reported yet, oldest
    // first.
    std::vector<const Entry *> unreported_results(Source source) const;
    bool mark_notified(std::uint32_t id);

    const std::vector<Entry> &entries() const
    {
        return entries_;
    }

    // Text form for the SD card: a version line with the next id, then one
    // line per entry. parse() replaces the contents; false (and an empty
    // queue) if the version is not recognised. Lines that cannot be read are
    // skipped.
    std::string serialize() const;
    bool parse(const std::string &text);

  private:
    Entry *find_mutable(std::uint32_t id);
    void make_room();

    std::vector<Entry> entries_;
    std::uint32_t next_id_ = 1;
};

// Cleans a key: only letters, digits and ":_-" stay, cut to kMaxKeyLength.
std::string sanitize_key(const std::string &key);

}  // namespace camera::action
