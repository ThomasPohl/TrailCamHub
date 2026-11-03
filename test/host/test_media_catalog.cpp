#include <doctest/doctest.h>

#include "media_catalog.hpp"
#include "media_list.hpp"

using namespace camera::artemis;

namespace
{
MediaItem item(std::int64_t dir, std::int64_t num, std::int64_t type,
               std::int64_t id, std::int64_t time = 0)
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
    REQUIRE(cat.set_flag(111, MediaCatalog::kThumbPresent));
    list.files.push_back(item(100, 40, 0, 333, 1766506500));
    CHECK(cat.merge(list) == 1);  // only the new one is added
    CHECK(cat.size() == 3);
    CHECK(cat.has_flag(111, MediaCatalog::kThumbPresent));  // flag kept
}

TEST_CASE("a reused dir/num after a card clear is a new recording")
{
    MediaCatalog cat;
    MediaList before;
    before.files = {item(100, 1, 0, 111, 1766400000),
                    item(100, 2, 2, 222, 1766400100)};
    cat.merge(before);
    cat.set_flag(111, MediaCatalog::kThumbPresent);
    cat.set_flag(111, MediaCatalog::kReported);
    cat.set_flag(222, MediaCatalog::kThumbPresent);
    cat.set_flag(222, MediaCatalog::kReported);

    // The card was cleared: the camera hands out 100_1 again, new mediaId.
    MediaList after;
    after.files = {item(100, 1, 0, 999, 1766500000)};
    CHECK(cat.merge(after) == 1);
    CHECK(cat.size() == 3);

    // The new recording starts with no flags, so it is fetched and reported.
    CHECK_FALSE(cat.has_flag(999, MediaCatalog::kThumbPresent));
    CHECK_FALSE(cat.has_flag(999, MediaCatalog::kReported));
    const auto reports = cat.without_flag(MediaCatalog::kReported, 10);
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].media_id == 999);

    // The old recordings keep their flags and data.
    CHECK(cat.has_flag(111, MediaCatalog::kThumbPresent));
    CHECK(cat.has_flag(111, MediaCatalog::kReported));
    REQUIRE(cat.find(111) != nullptr);
    CHECK(cat.find(111)->media_time == 1766400000);

    // Only the new one is on the card, so only it is camera work.
    const auto thumbs = cat.missing_thumbnails();
    REQUIRE(thumbs.size() == 1);
    CHECK(thumbs[0].media_id == 999);
    REQUIRE(cat.find_listed(100, 1) != nullptr);
    CHECK(cat.find_listed(100, 1)->media_id == 999);
    CHECK(cat.find_listed(100, 2) == nullptr);  // gone from the card
}

TEST_CASE("merge skips items without a valid mediaId")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 0), item(100, 2, 0, -5),
                  item(100, 3, 0, 333)};
    CHECK(cat.merge(list) == 1);
    CHECK(cat.size() == 1);
    CHECK(cat.find(333) != nullptr);
}

TEST_CASE("merge marks exactly the listed entries")
{
    MediaCatalog cat;
    MediaList first;
    first.files = {item(100, 1, 0, 111), item(100, 2, 0, 222)};
    cat.merge(first);
    CHECK(cat.find(111)->listed);
    CHECK(cat.find(222)->listed);

    MediaList second;
    second.files = {item(100, 2, 0, 222)};
    cat.merge(second);
    CHECK_FALSE(cat.find(111)->listed);
    CHECK(cat.find(222)->listed);
    // Not on the card: not queued for a thumbnail, even without one.
    REQUIRE(cat.missing_thumbnails().size() == 1);
    CHECK(cat.missing_thumbnails()[0].media_id == 222);
}

TEST_CASE("flag helpers operate on the addressed entry only")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11), item(100, 2, 2, 22)};
    cat.merge(list);

    CHECK(cat.set_flag(22, MediaCatalog::kVideoRequested));
    CHECK(cat.has_flag(22, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.has_flag(11, MediaCatalog::kVideoRequested));
    CHECK(cat.clear_flag(22, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.has_flag(22, MediaCatalog::kVideoRequested));
    CHECK_FALSE(cat.set_flag(99, MediaCatalog::kThumbPresent));  // absent
}

TEST_CASE("queries return the right work items")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11), item(100, 2, 2, 22),
                  item(100, 3, 2, 33)};
    cat.merge(list);
    cat.set_flag(11, MediaCatalog::kThumbPresent);
    cat.set_flag(22, MediaCatalog::kVideoRequested);

    CHECK(cat.missing_thumbnails().size() == 2);  // items 2 and 3
    REQUIRE(cat.pending_video_requests().size() == 1);
    CHECK(cat.pending_video_requests().front().media_num == 2);

    cat.set_flag(22, MediaCatalog::kFileDownloaded);
    CHECK(cat.pending_video_requests().empty());
    CHECK(cat.count_with_flag(MediaCatalog::kThumbPresent) == 1);
}

TEST_CASE("missing_downloads lists photos before videos")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 2, 11), item(100, 2, 0, 22), item(100, 3, 2, 33),
                  item(100, 4, 0, 44)};
    cat.merge(list);
    cat.set_flag(44, MediaCatalog::kFileDownloaded);

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
    cat.set_flag(2180198780, MediaCatalog::kThumbPresent);
    cat.set_flag(222, MediaCatalog::kVideoRequested);

    const std::string text = cat.serialize();
    CHECK(text.rfind("# trailcamhub-catalog v2\n", 0) == 0);
    MediaCatalog loaded;
    REQUIRE(loaded.parse(text));
    REQUIRE(loaded.size() == 2);
    const CatalogEntry *e = loaded.find(2180198780);
    REQUIRE(e != nullptr);
    CHECK(e->media_id == 2180198780);  // survives as int64
    CHECK(e->dir_num == 100);
    CHECK(e->media_num == 42);
    CHECK(e->media_time == 1766506634);
    CHECK_FALSE(e->listed);  // per session, not persisted
    CHECK(loaded.has_flag(2180198780, MediaCatalog::kThumbPresent));
    CHECK(loaded.has_flag(222, MediaCatalog::kVideoRequested));
}

TEST_CASE("serialize/parse keeps two recordings with the same dir/num")
{
    MediaCatalog cat;
    MediaList before;
    before.files = {item(100, 1, 0, 111)};
    cat.merge(before);
    cat.set_flag(111, MediaCatalog::kReported);
    MediaList after;
    after.files = {item(100, 1, 0, 999)};
    cat.merge(after);

    MediaCatalog loaded;
    REQUIRE(loaded.parse(cat.serialize()));
    CHECK(loaded.size() == 2);
    CHECK(loaded.has_flag(111, MediaCatalog::kReported));
    CHECK_FALSE(loaded.has_flag(999, MediaCatalog::kReported));
}

TEST_CASE("parse reads a v1 catalog")
{
    MediaCatalog cat;
    const std::string text = "# trailcamhub-catalog v1\n"
                             "100,42,111,0,1766506634,1\n"
                             "100,43,222,2,1766506600,17\n";
    REQUIRE(cat.parse(text));
    CHECK(cat.size() == 2);
    CHECK(cat.has_flag(111, MediaCatalog::kThumbPresent));
    CHECK(cat.has_flag(222, MediaCatalog::kReported));

    // Saved again as v2.
    CHECK(cat.serialize().rfind("# trailcamhub-catalog v2\n", 0) == 0);
}

TEST_CASE("parse rejects an unknown header and skips bad rows")
{
    MediaCatalog cat;
    CHECK_FALSE(cat.parse("garbage\n1,2,3,4,5,6\n"));
    CHECK_FALSE(cat.parse("# trailcamhub-catalog v3\n100,1,111,0,0,0\n"));

    MediaCatalog cat2;
    const std::string text = "# trailcamhub-catalog v2\n"
                             "100,42,111,0,1766506634,1\n"
                             "bad,row,here\n"               // too few fields
                             "100,x,1,0,0,0\n"              // non-numeric
                             "100,44,0,0,1766506000,0\n"    // no mediaId
                             "100,45,111,0,1766507000,0\n"  // repeated id
                             "100,43,222,2,1766506600,2\n";
    REQUIRE(cat2.parse(text));
    CHECK(cat2.size() == 2);
    REQUIRE(cat2.find(111) != nullptr);
    CHECK(cat2.find(111)->media_num == 42);  // the first row wins
}

TEST_CASE("without_flag and clear_flag_everywhere support delta reporting")
{
    MediaCatalog catalog;
    MediaList list;
    for (int num = 1; num <= 4; ++num)
        list.files.push_back(item(100, num, 0, 1000 + num));
    catalog.merge(list);

    std::vector<CatalogEntry> batch =
        catalog.without_flag(MediaCatalog::kReported, 3);
    REQUIRE(batch.size() == 3);
    for (const auto &e : batch)
        catalog.set_flag(e.media_id, MediaCatalog::kReported);

    batch = catalog.without_flag(MediaCatalog::kReported, 3);
    REQUIRE(batch.size() == 1);
    CHECK(batch[0].media_num == 4);

    // Flags survive a save/load round trip.
    MediaCatalog reloaded;
    REQUIRE(reloaded.parse(catalog.serialize()));
    CHECK(reloaded.count_with_flag(MediaCatalog::kReported) == 3);

    reloaded.clear_flag_everywhere(MediaCatalog::kReported);
    CHECK(reloaded.count_with_flag(MediaCatalog::kReported) == 0);
    CHECK(reloaded.without_flag(MediaCatalog::kReported, 10).size() == 4);
}
