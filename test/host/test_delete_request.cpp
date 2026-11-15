#include <doctest/doctest.h>

#include "action_queue.hpp"
#include "delete_request.hpp"

using namespace camera::action;
using uplink::decide_delete;
using uplink::delete_key;
using Kind = ActionQueue::AddResult::Kind;

namespace
{
const Action kDelete{Type::delete_media, 2035, "F4C15606ED3B"};

Outcome ok()
{
    Outcome o;
    o.done = true;
    return o;
}
}  // namespace

TEST_CASE("a backend deletion is queued once and answered when it ran")
{
    ActionQueue queue;
    const std::string key = delete_key("3f2c-11aa");
    CHECK(key == "be:3f2c-11aa");

    // First check-in: queued, a camera read is wanted, no answer yet.
    const auto first = queue.add(kDelete, Source::backend, key, 100);
    REQUIRE(first.kind == Kind::added);
    auto d = decide_delete(first, nullptr);
    CHECK_FALSE(d.answer);
    CHECK(d.read_needed);

    // The command comes again before the read: no second entry, no answer.
    auto again = queue.add(kDelete, Source::backend, key, 110);
    REQUIRE(again.kind == Kind::duplicate);
    d = decide_delete(again, queue.find(again.id));
    CHECK_FALSE(d.answer);
    CHECK_FALSE(d.read_needed);
    CHECK(queue.pending_count() == 1);

    // While the session runs it: wait.
    queue.start(first.id);
    d = decide_delete(queue.add(kDelete, Source::backend, key, 120),
                      queue.find(first.id));
    CHECK_FALSE(d.answer);

    // After the read: done.
    queue.finish(first.id, ok(), 130);
    again = queue.add(kDelete, Source::backend, key, 140);
    d = decide_delete(again, queue.find(again.id));
    CHECK(d.answer);
    CHECK(d.done);
    CHECK(d.error.empty());
}

TEST_CASE("a file the camera no longer lists counts as deleted")
{
    ActionQueue queue;
    const auto added = queue.add(kDelete, Source::backend, "be:1", 100);
    queue.start(added.id);
    queue.finish(added.id,
                 failure("the camera does not list this file any more"), 110);
    const auto d = decide_delete(
        queue.add(kDelete, Source::backend, "be:1", 120), queue.find(added.id));
    CHECK(d.answer);
    CHECK(d.done);
}

TEST_CASE("a failed deletion is answered with its reason")
{
    ActionQueue queue;
    const auto added = queue.add(kDelete, Source::backend, "be:2", 100);
    queue.start(added.id);
    queue.finish(added.id, failure("delRet -1"), 110);
    const auto d = decide_delete(
        queue.add(kDelete, Source::backend, "be:2", 120), queue.find(added.id));
    CHECK(d.answer);
    CHECK_FALSE(d.done);
    CHECK(d.error == "delRet -1");
}

TEST_CASE("a full queue and a refused deletion")
{
    ActionQueue::AddResult full;
    full.kind = Kind::full;
    auto d = decide_delete(full, nullptr);
    CHECK_FALSE(d.answer);

    ActionQueue queue;
    const auto refused = queue.add(Action{Type::delete_media, 0, "CAM"},
                                   Source::backend, "be:3", 1);
    REQUIRE(refused.kind == Kind::invalid);
    d = decide_delete(refused, nullptr);
    CHECK(d.answer);
    CHECK_FALSE(d.done);
    CHECK_FALSE(d.error.empty());
}
