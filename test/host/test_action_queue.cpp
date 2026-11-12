#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "action_queue.hpp"
#include "action_store.hpp"

using namespace camera::action;
using Kind = ActionQueue::AddResult::Kind;

namespace
{
const Action kPhoto{Type::photo, 0};
const Action kFormat{Type::format_sd, 0};
Action del(std::int64_t id)
{
    return Action{Type::delete_media, id};
}

struct TempDir {
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/trailcamhub-actions-XXXXXX";
        REQUIRE(mkdtemp(name) != nullptr);
        path = name;
    }
    ~TempDir()
    {
        remove((path + "/actions.csv").c_str());
        remove((path + "/actions.csv.tmp").c_str());
        rmdir(path.c_str());
    }
    std::string file() const
    {
        return path + "/actions.csv";
    }
};

Outcome ok()
{
    Outcome o;
    o.done = true;
    return o;
}
}  // namespace

TEST_CASE("actions run in the order they were asked for")
{
    ActionQueue queue;
    const auto a = queue.add(kPhoto, Source::console, "", 100);
    const auto b =
        queue.add(Action{Type::video_start, 0}, Source::telegram, "tg:1", 101);
    CHECK(a.kind == Kind::added);
    CHECK(b.kind == Kind::added);
    CHECK(a.id == 1);
    CHECK(b.id == 2);
    CHECK(queue.pending_count() == 2);

    REQUIRE(queue.next_pending() != nullptr);
    CHECK(queue.next_pending()->id == 1);
    CHECK(queue.start(1));
    CHECK(queue.find(1)->state == State::running);
    // A running action is not offered again, and cannot be started twice.
    CHECK(queue.next_pending()->id == 2);
    CHECK_FALSE(queue.start(1));
    CHECK(queue.finish(1, ok(), 110));
    CHECK(queue.find(1)->state == State::done);
    CHECK(queue.find(1)->finished_s == 110);
    // Finished once.
    CHECK_FALSE(queue.finish(1, ok(), 111));
}

TEST_CASE("a failed action keeps its reason without commas")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::console, "", 100);
    CHECK(queue.finish(1, failure("the camera, answered; snapRet -2\n"), 105));
    const Entry *entry = queue.find(1);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == State::failed);
    CHECK(entry->reason == "the camera; answered; snapRet -2");
}

TEST_CASE("a command that arrives twice is queued once")
{
    ActionQueue queue;
    const auto first = queue.add(kPhoto, Source::telegram, "tg:4711", 100);
    const auto second = queue.add(kPhoto, Source::telegram, "tg:4711", 101);
    CHECK(first.kind == Kind::added);
    CHECK(second.kind == Kind::duplicate);
    CHECK(second.id == first.id);
    CHECK(queue.pending_count() == 1);

    // Also after it was done: the key is still known.
    queue.start(first.id);
    queue.finish(first.id, ok(), 110);
    CHECK(queue.add(kPhoto, Source::telegram, "tg:4711", 120).kind ==
          Kind::duplicate);
    // Another update is another command.
    CHECK(queue.add(kPhoto, Source::telegram, "tg:4712", 120).kind ==
          Kind::added);
}

TEST_CASE("the same deletion or format is not queued twice while it waits")
{
    ActionQueue queue;
    CHECK(queue.add(del(111), Source::console, "", 1).kind == Kind::added);
    CHECK(queue.add(del(111), Source::web_ui, "", 2).kind == Kind::duplicate);
    CHECK(queue.add(del(222), Source::console, "", 3).kind == Kind::added);
    CHECK(queue.add(kFormat, Source::console, "", 4).kind == Kind::added);
    CHECK(queue.add(kFormat, Source::console, "", 5).kind == Kind::duplicate);
    // Two photos are two photos.
    CHECK(queue.add(kPhoto, Source::console, "", 6).kind == Kind::added);
    CHECK(queue.add(kPhoto, Source::console, "", 7).kind == Kind::added);
    CHECK(queue.has_pending_of(Type::format_sd));
    CHECK(queue.has_pending_needing_list());
}

TEST_CASE("invalid and forbidden actions are refused")
{
    ActionQueue queue;
    const auto bad = queue.add(del(0), Source::console, "", 1);
    CHECK(bad.kind == Kind::invalid);
    CHECK_FALSE(bad.reason.empty());
    const auto remote = queue.add(kFormat, Source::telegram, "", 1);
    CHECK(remote.kind == Kind::not_allowed);
    CHECK(queue.add(del(5), Source::backend, "", 1).kind == Kind::not_allowed);
    CHECK(queue.pending_count() == 0);
}

TEST_CASE("only a few actions may wait")
{
    ActionQueue queue;
    for (std::size_t i = 0; i < ActionQueue::kMaxPending; ++i) {
        CHECK(queue.add(kPhoto, Source::telegram, "k" + std::to_string(i), 1)
                  .kind == Kind::added);
    }
    CHECK(queue.add(kPhoto, Source::telegram, "more", 1).kind == Kind::full);
    // Room again once one is done.
    queue.start(1);
    queue.finish(1, ok(), 2);
    CHECK(queue.add(kPhoto, Source::telegram, "more", 3).kind == Kind::added);
}

TEST_CASE("the oldest finished entry makes room")
{
    ActionQueue queue;
    for (std::size_t i = 0; i < ActionQueue::kMaxEntries + 5; ++i) {
        const auto added = queue.add(kPhoto, Source::console, "", 10);
        REQUIRE(added.kind == Kind::added);
        queue.start(added.id);
        queue.finish(added.id, ok(), 11);
    }
    CHECK(queue.entries().size() == ActionQueue::kMaxEntries);
    CHECK(queue.find(1) == nullptr);
    CHECK(queue.find(ActionQueue::kMaxEntries + 5) != nullptr);
}

TEST_CASE("a restart does not repeat an action that was running")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::console, "", 100);
    queue.add(del(5), Source::console, "", 101);
    queue.start(2);
    CHECK(queue.abandon_running("interrupted", 200) == 1);
    CHECK(queue.find(2)->state == State::failed);
    CHECK(queue.find(2)->reason == "interrupted");
    CHECK(queue.find(1)->state == State::pending);
}

TEST_CASE("an action that waited more than a day is dropped")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::telegram, "a", 1000);
    queue.add(kPhoto, Source::telegram, "b", 1000 + 20 * 3600);
    // No clock: nothing is judged.
    CHECK(queue.expire_stale(0) == 0);
    CHECK(queue.expire_stale(1000 + ActionQueue::kMaxAgeS + 1) == 1);
    CHECK(queue.find(1)->state == State::failed);
    CHECK(queue.find(2)->state == State::pending);
    // Entries queued without a clock never expire by age.
    ActionQueue unset;
    unset.add(kPhoto, Source::console, "", 0);
    CHECK(unset.expire_stale(5000000) == 0);
}

TEST_CASE("all waiting actions can fail together")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::console, "", 1);
    queue.add(kFormat, Source::console, "", 1);
    queue.start(1);
    CHECK(queue.fail_pending("the camera has no SD card", 5) == 1);
    CHECK(queue.find(1)->state == State::running);
    CHECK(queue.find(2)->state == State::failed);
}

TEST_CASE("results are reported once to the front end that asked")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::telegram, "t1", 1);
    queue.add(kPhoto, Source::console, "", 1);
    queue.add(kPhoto, Source::telegram, "t2", 1);
    queue.finish(1, ok(), 5);
    queue.finish(2, failure("x"), 5);
    auto results = queue.unreported_results(Source::telegram);
    REQUIRE(results.size() == 1);
    CHECK(results[0]->id == 1);
    CHECK(queue.mark_notified(1));
    CHECK(queue.unreported_results(Source::telegram).empty());
    CHECK(queue.unreported_results(Source::console).size() == 1);
    CHECK_FALSE(queue.mark_notified(99));
}

TEST_CASE("keys are cleaned")
{
    CHECK(sanitize_key("tg:42_x-1") == "tg:42_x-1");
    CHECK(sanitize_key("a,b\nc d") == "abcd");
    CHECK(sanitize_key(std::string(100, 'a')).size() ==
          ActionQueue::kMaxKeyLength);
}

TEST_CASE("the queue survives a round trip through text")
{
    ActionQueue queue;
    queue.add(kPhoto, Source::telegram, "tg:1", 100);
    queue.add(del(4294967295LL), Source::console, "", 101);
    queue.add(kFormat, Source::web_ui, "", 102);
    queue.start(1);
    queue.finish(1, failure("snapRet -2, card full"), 110);
    queue.mark_notified(1);

    ActionQueue loaded;
    REQUIRE(loaded.parse(queue.serialize()));
    REQUIRE(loaded.entries().size() == 3);
    const Entry &first = loaded.entries()[0];
    CHECK(first.state == State::failed);
    CHECK(first.reason == "snapRet -2; card full");
    CHECK(first.key == "tg:1");
    CHECK(first.notified);
    CHECK(first.source == Source::telegram);
    CHECK(loaded.entries()[1].action.media_id == 4294967295LL);
    CHECK(loaded.entries()[2].source == Source::web_ui);
    // Ids go on where they were.
    CHECK(loaded.add(kPhoto, Source::console, "", 200).id == 4);
    CHECK(loaded.serialize() != queue.serialize());
}

TEST_CASE("unreadable text and lines are handled")
{
    ActionQueue queue;
    CHECK_FALSE(queue.parse(""));
    CHECK_FALSE(queue.parse("v9,1\n"));
    CHECK_FALSE(queue.parse("v1,0\n"));
    CHECK(queue.entries().empty());

    // A broken line is skipped, the others stay; ids never go back.
    REQUIRE(queue.parse("v1,3\n"
                        "1,photo,0,console,done,10,11,0,,\n"
                        "garbage\n"
                        "2,photo,0,nobody,done,10,11,0,,\n"
                        "7,delete,0,console,pending,10,0,0,,\n"
                        "5,photo,0,telegram,pending,10,0,0,k,\r\n"));
    REQUIRE(queue.entries().size() == 2);
    CHECK(queue.entries()[0].id == 1);
    CHECK(queue.entries()[1].id == 5);
    CHECK(queue.add(kPhoto, Source::console, "", 1).id == 6);
}

TEST_CASE(
    "the file: missing is empty, saved is loaded, running is not repeated")
{
    TempDir dir;
    ActionQueue queue;
    CHECK(load_queue(dir.file(), queue, 100));
    CHECK(queue.entries().empty());

    queue.add(kPhoto, Source::telegram, "tg:9", 100);
    queue.add(kFormat, Source::console, "", 100);
    queue.start(1);
    REQUIRE(save_queue(dir.file(), queue));

    ActionQueue loaded;
    REQUIRE(load_queue(dir.file(), loaded, 500));
    REQUIRE(loaded.entries().size() == 2);
    CHECK(loaded.find(1)->state == State::failed);
    CHECK(loaded.find(1)->finished_s == 500);
    CHECK(loaded.find(2)->state == State::pending);
    CHECK(loaded.add(kPhoto, Source::telegram, "tg:9", 600).kind ==
          Kind::duplicate);
}

TEST_CASE("a power loss between the remove and the rename keeps the queue")
{
    TempDir dir;
    ActionQueue queue;
    queue.add(kPhoto, Source::console, "", 100);
    REQUIRE(save_queue(dir.file(), queue));
    // The new file is complete, the old one is gone, the rename did not
    // happen.
    REQUIRE(rename(dir.file().c_str(), (dir.file() + ".tmp").c_str()) == 0);

    ActionQueue loaded;
    REQUIRE(load_queue(dir.file(), loaded, 200));
    CHECK(loaded.entries().size() == 1);
}

TEST_CASE("an unreadable file is reported and leaves the queue empty")
{
    TempDir dir;
    FILE *f = fopen(dir.file().c_str(), "wb");
    REQUIRE(f != nullptr);
    fputs("not a queue\n", f);
    fclose(f);
    ActionQueue queue;
    queue.add(kPhoto, Source::console, "", 1);
    CHECK_FALSE(load_queue(dir.file(), queue, 2));
    CHECK(queue.entries().empty());
}
