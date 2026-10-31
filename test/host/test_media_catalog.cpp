#include <doctest/doctest.h>

#include "media_catalog.hpp"
#include "media_list.hpp"

using namespace camera::artemis;

namespace
{
MediaItem item(std::int64_t dir, std::int64_t num, std::int64_t type,
               std::int64_t id = 0, std::int64_t time = 0)
{
    MediaItem m;
    m.dir_num = dir;
    m.num = num;
    m.file_type = type;
    m.id = id;
    m.time = time;
    return m;
}
}  // namespace

TEST_CASE("merge adds new items and preserves flags on re-merge")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 42, 0, 111, 1766506634),
                  item(100, 41, 2, 222, 1766506600)};
    CHECK(cat.merge(list) == 2);
    CHECK(cat.size() == 2);

    // Mark one thumbnail stored, then re-fetch the same list plus a new item.
    REQUIRE(cat.set_flag(100, 42, MediaCatalog::kThumbPresent));
    list.files.push_back(item(100, 40, 0, 333, 1766506500));
    CHECK(cat.merge(list) == 1);  // only the new one is added
    CHECK(cat.size() == 3);
    CHECK(cat.has_flag(100, 42, MediaCatalog::kThumbPresent));  // flag kept
}

TEST_CASE("flag helpers operate on the addressed entry only")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0), item(100, 2, 2)};
    cat.merge(list);

    CHECK(cat.set_flag(100, 2, MediaCatalog::kVideoRequested));
    CHECK(cat.has_flag(100, 2, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.has_flag(100, 1, MediaCatalog::kVideoRequested));
    CHECK(cat.clear_flag(100, 2, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.has_flag(100, 2, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.set_flag(100, 99, MediaCatalog::kThumbPresent));  // absent
}

TEST_CASE("queries return the right work items")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0), item(100, 2, 2), item(100, 3, 2)};
    cat.merge(list);
    cat.set_flag(100, 1, MediaCatalog::kThumbPresent);
    cat.set_flag(100, 2, MediaCatalog::kVideoRequested);

    CHECK(cat.missing_thumbnails().size() == 2);  // items 2 and 3
    REQUIRE(cat.pending_video_requests().size() == 1);
    CHECK(cat.pending_video_requests().front().media_num == 2);

    cat.set_flag(100, 2, MediaCatalog::kFileDownloaded);
    CHECK(cat.pending_video_requests().empty());
    CHECK(cat.count_with_flag(MediaCatalog::kThumbPresent) == 1);
}

TEST_CASE("missing_downloads lists photos before videos")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 2), item(100, 2, 0), item(100, 3, 2),
                  item(100, 4, 0)};
    cat.merge(list);
    cat.set_flag(100, 4, MediaCatalog::kFileDownloaded);

    const auto both = cat.missing_downloads(true, true);
    REQUIRE(both.size() == 3);
    CHECK(both[0].media_num == 2);
    CHECK(both[1].media_num == 1);
    CHECK(both[2].media_num == 3);
    CHECK(cat.missing_downloads(true, false).size() == 1);
    CHECK(cat.missing_downloads(false, true).size() == 2);
    CHECK(cat.missing_downloads(false, false).empty());
}

TEST_CASE("serialize/parse round-trips the catalog")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 42, 0, 2180198780, 1766506634),
                  item(100, 41, 2, 222, 1766506600)};
    cat.merge(list);
    cat.set_flag(100, 42, MediaCatalog::kThumbPresent);
    cat.set_flag(100, 41, MediaCatalog::kVideoRequested);

    const std::string text = cat.serialize();
    MediaCatalog loaded;
    REQUIRE(loaded.parse(text));
    REQUIRE(loaded.size() == 2);
    const CatalogEntry *e = loaded.find(100, 42);
    REQUIRE(e != nullptr);
    CHECK(e->media_id == 2180198780);  // survives as int64
    CHECK(e->media_time == 1766506634);
    CHECK(loaded.has_flag(100, 42, MediaCatalog::kThumbPresent));
    CHECK(loaded.has_flag(100, 41, MediaCatalog::kVideoRequested));
}

TEST_CASE("parse rejects an unknown header and skips malformed rows")
{
    MediaCatalog cat;
    CHECK_FALSE(cat.parse("garbage\n1,2,3,4,5,6\n"));

    MediaCatalog cat2;
    const std::string text = "# trailcamhub-catalog v1\n"
                             "100,42,111,0,1766506634,1\n"
                             "bad,row,here\n"   // too few fields -> skipped
                             "100,x,1,0,0,0\n"  // non-numeric -> skipped
                             "100,43,222,2,1766506600,2\n";
    REQUIRE(cat2.parse(text));
    CHECK(cat2.size() == 2);
}

TEST_CASE("without_flag and clear_flag_everywhere support delta reporting")
{
    camera::artemis::MediaCatalog catalog;
    camera::artemis::MediaList list;
    for (int num = 1; num <= 4; ++num) {
        camera::artemis::MediaItem item;
        item.dir_num = 100;
        item.num = num;
        list.files.push_back(item);
    }
    catalog.merge(list);
    using Flag = camera::artemis::MediaCatalog;

    std::vector<camera::artemis::CatalogEntry> batch =
        catalog.without_flag(Flag::kReported, 3);
    REQUIRE(batch.size() == 3);
    for (const auto &e : batch)
        catalog.set_flag(e.dir_num, e.media_num, Flag::kReported);

    batch = catalog.without_flag(Flag::kReported, 3);
    REQUIRE(batch.size() == 1);
    CHECK(batch[0].media_num == 4);

    // Flags survive a save/load round trip.
    camera::artemis::MediaCatalog reloaded;
    REQUIRE(reloaded.parse(catalog.serialize()));
    CHECK(reloaded.count_with_flag(Flag::kReported) == 3);

    reloaded.clear_flag_everywhere(Flag::kReported);
    CHECK(reloaded.count_with_flag(Flag::kReported) == 0);
    CHECK(reloaded.without_flag(Flag::kReported, 10).size() == 4);
}
