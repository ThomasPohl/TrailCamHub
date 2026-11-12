#include "action_queue.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace camera::action
{

namespace
{

constexpr const char *kHeader = "v1";

std::string clean_reason(const std::string &reason)
{
    std::string out;
    for (const char c : reason) {
        if (out.size() >= ActionQueue::kMaxReasonLength) {
            break;
        }
        if (c == ',' || c == ';') {
            out += ';';
        } else if (std::isprint(static_cast<unsigned char>(c))) {
            out += c;
        }
    }
    return out;
}

bool is_finished(State state)
{
    return state == State::done || state == State::failed;
}

std::optional<State> parse_state(const std::string &name)
{
    for (const State state :
         {State::pending, State::running, State::done, State::failed}) {
        if (name == state_name(state)) {
            return state;
        }
    }
    return std::nullopt;
}

std::optional<Source> parse_source(const std::string &name)
{
    for (const Source source :
         {Source::console, Source::web_ui, Source::telegram, Source::backend}) {
        if (name == source_name(source)) {
            return source;
        }
    }
    return std::nullopt;
}

bool to_int(const std::string &text, std::int64_t &out)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    out = std::strtoll(text.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

}  // namespace

const char *state_name(State state)
{
    switch (state) {
    case State::pending:
        return "pending";
    case State::running:
        return "running";
    case State::done:
        return "done";
    case State::failed:
        return "failed";
    }
    return "unknown";
}

std::string describe(const Entry &entry)
{
    std::string text = type_name(entry.action.type);
    if (entry.action.media_id > 0) {
        text +=
            " " + std::to_string(static_cast<long long>(entry.action.media_id));
    }
    text += std::string(": ") + state_name(entry.state);
    if (!entry.reason.empty()) {
        text += ": " + entry.reason;
    }
    return text;
}

std::string sanitize_key(const std::string &key)
{
    std::string out;
    for (const char c : key) {
        if (out.size() >= ActionQueue::kMaxKeyLength) {
            break;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ':' ||
            c == '_' || c == '-') {
            out += c;
        }
    }
    return out;
}

Entry *ActionQueue::find_mutable(std::uint32_t id)
{
    for (Entry &entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const Entry *ActionQueue::find(std::uint32_t id) const
{
    for (const Entry &entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

std::size_t ActionQueue::pending_count() const
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const Entry &e) {
            return e.state == State::pending;
        }));
}

bool ActionQueue::has_pending_of(Type type) const
{
    return std::any_of(
        entries_.begin(), entries_.end(), [type](const Entry &e) {
            return e.state == State::pending && e.action.type == type;
        });
}

bool ActionQueue::has_pending_needing_list() const
{
    return std::any_of(entries_.begin(), entries_.end(), [](const Entry &e) {
        return e.state == State::pending && needs_media_list(e.action.type);
    });
}

void ActionQueue::make_room()
{
    // The oldest finished entry goes; entries are in the order they were
    // added.
    while (entries_.size() >= kMaxEntries) {
        const auto oldest =
            std::find_if(entries_.begin(), entries_.end(),
                         [](const Entry &e) { return is_finished(e.state); });
        if (oldest == entries_.end()) {
            return;
        }
        entries_.erase(oldest);
    }
}

ActionQueue::AddResult ActionQueue::add(const Action &action, Source source,
                                        const std::string &key,
                                        std::int64_t now_s)
{
    AddResult result;
    const std::string problem = validate(action);
    if (!problem.empty()) {
        result.kind = AddResult::Kind::invalid;
        result.reason = problem;
        return result;
    }
    if (!allowed(action.type, source)) {
        result.kind = AddResult::Kind::not_allowed;
        result.reason = std::string("the ") + type_name(action.type) +
                        " action is not allowed from " + source_name(source);
        return result;
    }
    const std::string clean_key = sanitize_key(key);
    for (const Entry &entry : entries_) {
        const bool same_key = !clean_key.empty() && entry.key == clean_key;
        // The same thing asked for again while it waits: once is enough, and
        // two deletions of one file or two formats in a row would be a hazard.
        const bool same_pending = entry.state == State::pending &&
                                  entry.action.type == action.type &&
                                  entry.action.media_id == action.media_id &&
                                  (action.type == Type::delete_media ||
                                   action.type == Type::format_sd);
        if (same_key || same_pending) {
            result.kind = AddResult::Kind::duplicate;
            result.id = entry.id;
            return result;
        }
    }
    if (pending_count() >= kMaxPending) {
        result.kind = AddResult::Kind::full;
        return result;
    }
    make_room();
    Entry entry;
    entry.id = next_id_++;
    entry.action = action;
    entry.source = source;
    entry.key = clean_key;
    entry.created_s = now_s > 0 ? now_s : 0;
    entries_.push_back(entry);
    result.kind = AddResult::Kind::added;
    result.id = entry.id;
    return result;
}

const Entry *ActionQueue::next_pending() const
{
    for (const Entry &entry : entries_) {
        if (entry.state == State::pending) {
            return &entry;
        }
    }
    return nullptr;
}

bool ActionQueue::start(std::uint32_t id)
{
    Entry *entry = find_mutable(id);
    if (entry == nullptr || entry->state != State::pending) {
        return false;
    }
    entry->state = State::running;
    return true;
}

bool ActionQueue::finish(std::uint32_t id, const Outcome &outcome,
                         std::int64_t now_s)
{
    Entry *entry = find_mutable(id);
    if (entry == nullptr || is_finished(entry->state)) {
        return false;
    }
    entry->state = outcome.done ? State::done : State::failed;
    entry->reason = outcome.done ? "" : clean_reason(outcome.reason);
    entry->finished_s = now_s > 0 ? now_s : 0;
    entry->notified = false;
    return true;
}

std::size_t ActionQueue::abandon_running(const std::string &reason,
                                         std::int64_t now_s)
{
    std::size_t count = 0;
    for (Entry &entry : entries_) {
        if (entry.state == State::running) {
            entry.state = State::failed;
            entry.reason = clean_reason(reason);
            entry.finished_s = now_s > 0 ? now_s : 0;
            ++count;
        }
    }
    return count;
}

std::size_t ActionQueue::expire_stale(std::int64_t now_s)
{
    if (now_s <= 0) {
        return 0;
    }
    std::size_t count = 0;
    for (Entry &entry : entries_) {
        if (entry.state == State::pending && entry.created_s > 0 &&
            now_s - entry.created_s > kMaxAgeS) {
            entry.state = State::failed;
            entry.reason = "waited too long for a camera read";
            entry.finished_s = now_s;
            ++count;
        }
    }
    return count;
}

std::size_t ActionQueue::fail_pending(const std::string &reason,
                                      std::int64_t now_s)
{
    std::size_t count = 0;
    for (Entry &entry : entries_) {
        if (entry.state == State::pending) {
            entry.state = State::failed;
            entry.reason = clean_reason(reason);
            entry.finished_s = now_s > 0 ? now_s : 0;
            ++count;
        }
    }
    return count;
}

std::vector<const Entry *> ActionQueue::unreported_results(Source source) const
{
    std::vector<const Entry *> out;
    for (const Entry &entry : entries_) {
        if (is_finished(entry.state) && !entry.notified &&
            entry.source == source) {
            out.push_back(&entry);
        }
    }
    return out;
}

bool ActionQueue::mark_notified(std::uint32_t id)
{
    Entry *entry = find_mutable(id);
    if (entry == nullptr) {
        return false;
    }
    entry->notified = true;
    return true;
}

// id,type,mediaId,source,state,created,finished,notified,key,reason
std::string ActionQueue::serialize() const
{
    std::ostringstream out;
    out << kHeader << ',' << next_id_ << '\n';
    for (const Entry &e : entries_) {
        out << e.id << ',' << type_name(e.action.type) << ','
            << e.action.media_id << ',' << source_name(e.source) << ','
            << state_name(e.state) << ',' << e.created_s << ',' << e.finished_s
            << ',' << (e.notified ? 1 : 0) << ',' << e.key << ',' << e.reason
            << '\n';
    }
    return out.str();
}

bool ActionQueue::parse(const std::string &text)
{
    entries_.clear();
    next_id_ = 1;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    const std::size_t comma = line.find(',');
    std::int64_t next = 0;
    if (comma == std::string::npos || line.substr(0, comma) != kHeader ||
        !to_int(line.substr(comma + 1), next) || next < 1 ||
        next > 0x7fffffff) {
        return false;
    }
    next_id_ = static_cast<std::uint32_t>(next);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Nine fields, then the reason (which has no commas).
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (fields.size() < 9) {
            const std::size_t end = line.find(',', start);
            if (end == std::string::npos) {
                break;
            }
            fields.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        if (fields.size() < 9) {
            continue;
        }
        Entry entry;
        std::int64_t id = 0;
        std::int64_t media = 0;
        std::int64_t created = 0;
        std::int64_t finished = 0;
        std::int64_t notified = 0;
        const auto type = parse_type(fields[1]);
        const auto source = parse_source(fields[3]);
        const auto state = parse_state(fields[4]);
        if (!to_int(fields[0], id) || id < 1 || id > 0x7fffffff || !type ||
            !to_int(fields[2], media) || !source || !state ||
            !to_int(fields[5], created) || !to_int(fields[6], finished) ||
            !to_int(fields[7], notified)) {
            continue;
        }
        entry.id = static_cast<std::uint32_t>(id);
        entry.action = Action{*type, media};
        entry.source = *source;
        entry.state = *state;
        entry.created_s = created;
        entry.finished_s = finished;
        entry.notified = notified != 0;
        entry.key = sanitize_key(fields[8]);
        entry.reason = clean_reason(line.substr(start));
        if (!validate(entry.action).empty()) {
            continue;
        }
        entries_.push_back(entry);
        if (entry.id >= next_id_) {
            next_id_ = entry.id + 1;
        }
    }
    return true;
}

}  // namespace camera::action
