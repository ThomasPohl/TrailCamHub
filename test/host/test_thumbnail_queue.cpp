#include <doctest/doctest.h>

#include <vector>

#include "thumbnail_queue.hpp"

using camera::artemis::CatalogEntry;
using camera::link::ThumbnailQueue;

namespace
{
CatalogEntry entry(std::int64_t dir, std::int64_t num, std::int64_t type)
{
    CatalogEntry e;
    e.dir_num = dir;
    e.media_num = num;
    e.media_id = dir * 1000 + num;
    e.file_type = type;
    return e;
}

std::vector<CatalogEntry> three()
{
    return {entry(100, 7, 0), entry(100, 6, 2), entry(100, 5, 0)};
}
}  // namespace

TEST_CASE("an empty thumbnail queue has nothing to request")
{
    ThumbnailQueue queue;
    CHECK(queue.current() == nullptr);
    CHECK(queue.request_fields().empty());
    CHECK(queue.size() == 0);
    // Nothing to skip either.
    queue.advance();
    CHECK(queue.handled() == 0);
}

TEST_CASE("thumbnails are requested in the given order")
{
    ThumbnailQueue queue;
    queue.start(three());
    CHECK(queue.size() == 3);
    REQUIRE(queue.current() != nullptr);
    CHECK(queue.current()->media_num == 7);
    queue.advance();
    CHECK(queue.current()->media_num == 6);
    queue.advance();
    CHECK(queue.current()->media_num == 5);
    queue.advance();
    CHECK(queue.current() == nullptr);
    CHECK(queue.handled() == 3);
}

TEST_CASE("the request names file type, directory and number")
{
    ThumbnailQueue queue;
    queue.start(three());
    CHECK(queue.request_fields() == ",\"thumbnailReqs\":[{\"fileType\":0,"
                                    "\"dirNum\":100,\"mediaNum\":7}]");
    queue.advance();
    CHECK(queue.request_fields() == ",\"thumbnailReqs\":[{\"fileType\":2,"
                                    "\"dirNum\":100,\"mediaNum\":6}]");
}

TEST_CASE("a stalled thumbnail is requested again once, then skipped")
{
    ThumbnailQueue queue;
    queue.start(three());
    CHECK_FALSE(queue.retrying());
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::request_again);
    CHECK(queue.retrying());
    CHECK(queue.current()->media_num == 7);
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::skipped);
    CHECK(queue.current()->media_num == 6);
    // The next thumbnail gets its own retry.
    CHECK_FALSE(queue.retrying());
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::request_again);
}

TEST_CASE("a thumbnail that arrives after a retry clears the retry")
{
    ThumbnailQueue queue;
    queue.start(three());
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::request_again);
    queue.advance();
    CHECK(queue.current()->media_num == 6);
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::request_again);
}

TEST_CASE("skipping the last thumbnail ends the queue")
{
    ThumbnailQueue queue;
    queue.start({entry(100, 1, 0)});
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::request_again);
    CHECK(queue.on_stall() == ThumbnailQueue::Stall::skipped);
    CHECK(queue.current() == nullptr);
    CHECK(queue.handled() == 1);
}

TEST_CASE("start and release reset the queue")
{
    ThumbnailQueue queue;
    queue.start(three());
    queue.on_stall();
    queue.advance();
    queue.start({entry(101, 1, 0)});
    CHECK(queue.handled() == 0);
    CHECK_FALSE(queue.retrying());
    CHECK(queue.current()->dir_num == 101);
    queue.on_stall();
    queue.release();
    CHECK(queue.size() == 0);
    CHECK(queue.current() == nullptr);
    CHECK_FALSE(queue.retrying());
}
