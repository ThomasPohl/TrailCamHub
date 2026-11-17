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

TEST_CASE("missing_downloads lists photos before videos, each newest first")
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
    CHECK(both[1].media_num == 3);
    CHECK(both[2].media_num == 1);
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
    CHECK(text.rfind("# trailcamhub-catalog v4\n", 0) == 0);
    MediaCatalog loaded;
    REQUIRE(loaded.parse(text));
    CHECK(loaded.format_version() == 4);
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
    CHECK(cat.format_version() == 1);
    CHECK(cat.size() == 2);
    CHECK(cat.has_flag(111, MediaCatalog::kThumbPresent));
    CHECK(cat.has_flag(222, MediaCatalog::kReported));

    // Saved again as the current version.
    CHECK(cat.serialize().rfind("# trailcamhub-catalog v4\n", 0) == 0);
}

TEST_CASE("parse reports the version it read")
{
    MediaCatalog cat;
    CHECK(cat.format_version() == 0);
    REQUIRE(cat.parse("# trailcamhub-catalog v2\n100,42,111,0,0,1\n"));
    CHECK(cat.format_version() == 2);
    REQUIRE(cat.parse("# trailcamhub-catalog v3\n100,42,111,0,0,1\n"));
    CHECK(cat.format_version() == MediaCatalog::kFormatVersion);
    cat.clear();
    CHECK(cat.format_version() == 0);
}

TEST_CASE("parse rejects an unknown header and skips bad rows")
{
    MediaCatalog cat;
    CHECK_FALSE(cat.parse("garbage\n1,2,3,4,5,6\n"));
    CHECK_FALSE(cat.parse("# trailcamhub-catalog v5\n100,1,111,0,0,0\n"));
    CHECK(cat.format_version() == 0);

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

TEST_CASE("merge_page keeps the entries of earlier pages listed")
{
    MediaCatalog cat;
    MediaList old_list;
    old_list.files = {item(100, 1, 0, 11), item(100, 2, 0, 22)};
    cat.merge(old_list);

    // Two newer recordings arrived; the list now spans two pages and the
    // known ones moved to page 1.
    cat.begin_listing();
    MediaList page0;
    page0.files = {item(100, 4, 0, 44), item(100, 3, 0, 33)};
    MediaList page1;
    page1.files = {item(100, 2, 0, 22), item(100, 1, 0, 11)};
    CHECK(cat.merge_page(page0) == 2);
    CHECK(cat.merge_page(page1) == 0);

    for (const CatalogEntry &e : cat.entries())
        CHECK(e.listed);
    CHECK(cat.missing_thumbnails().size() == 4);
}

TEST_CASE("begin_listing unlists entries that no page contains")
{
    MediaCatalog cat;
    MediaList old_list;
    old_list.files = {item(100, 1, 0, 11), item(100, 2, 0, 22)};
    cat.merge(old_list);

    cat.begin_listing();
    MediaList page0;
    page0.files = {item(100, 2, 0, 22)};
    cat.merge_page(page0);

    CHECK(cat.find(22)->listed);
    CHECK_FALSE(cat.find(11)->listed);  // deleted from the card
    REQUIRE(cat.missing_thumbnails().size() == 1);
    CHECK(cat.missing_thumbnails()[0].media_id == 22);
}

TEST_CASE("camera work is ordered newest first, also across catalog order")
{
    MediaCatalog cat;
    MediaList first;
    first.files = {item(100, 2, 0, 22), item(100, 1, 2, 11)};
    cat.merge(first);
    // A later session appends newer recordings at the end of the catalog,
    // including one in the next DCIM directory.
    MediaList second;
    second.files = {item(101, 1, 0, 1011), item(100, 3, 2, 33),
                    item(100, 2, 0, 22), item(100, 1, 2, 11)};
    cat.merge(second);
    cat.set_flag(11, MediaCatalog::kVideoRequested);
    cat.set_flag(33, MediaCatalog::kVideoRequested);

    const auto thumbs = cat.missing_thumbnails();
    REQUIRE(thumbs.size() == 4);
    CHECK(thumbs[0].media_id == 1011);
    CHECK(thumbs[1].media_id == 33);
    CHECK(thumbs[2].media_id == 22);
    CHECK(thumbs[3].media_id == 11);

    const auto videos = cat.pending_video_requests();
    REQUIRE(videos.size() == 2);
    CHECK(videos[0].media_id == 33);
    CHECK(videos[1].media_id == 11);
}

TEST_CASE("unlisted entries are gone after a complete list and reported once")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11), item(100, 2, 2, 22),
                  item(100, 3, 0, 33)};
    cat.merge(list);
    REQUIRE(cat.set_flag(11, MediaCatalog::kReported));
    REQUIRE(cat.set_flag(22, MediaCatalog::kReported));

    // 22 was deleted on the camera.
    list.files = {item(100, 1, 0, 11), item(100, 3, 0, 33)};
    cat.merge(list);
    CHECK(cat.mark_unlisted_gone() == 1);
    CHECK(cat.mark_unlisted_gone() == 0);  // already marked
    CHECK(cat.has_flag(22, MediaCatalog::kGone));

    const auto present = cat.unreported_present(10);
    REQUIRE(present.size() == 1);
    CHECK(present[0].media_id == 33);
    CHECK(cat.unreported_gone(10) == std::vector<std::int64_t>{22});

    REQUIRE(cat.set_flag(22, MediaCatalog::kGoneReported));
    CHECK(cat.unreported_gone(10).empty());

    // After a resync the gone entry is not reported as media again.
    cat.clear_flag_everywhere(MediaCatalog::kReported);
    cat.clear_flag_everywhere(MediaCatalog::kGoneReported);
    const auto again = cat.unreported_present(10);
    CHECK(again.size() == 2);
    for (const auto &e : again) {
        CHECK(e.media_id != 22);
    }
    CHECK(cat.unreported_gone(10) == std::vector<std::int64_t>{22});
}

TEST_CASE("a deletion by the hub marks the entry gone")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11)};
    cat.merge(list);
    CHECK(cat.mark_gone(11));
    CHECK_FALSE(cat.mark_gone(11));
    CHECK_FALSE(cat.mark_gone(99));
    CHECK(cat.unreported_present(10).empty());
    CHECK(cat.unreported_gone(10) == std::vector<std::int64_t>{11});
    CHECK(cat.missing_thumbnails().empty());
}

TEST_CASE("an entry listed again is no longer gone and is reported again")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11)};
    cat.merge(list);
    REQUIRE(cat.set_flag(11, MediaCatalog::kReported));
    REQUIRE(cat.mark_gone(11));
    REQUIRE(cat.set_flag(11, MediaCatalog::kGoneReported));
    cat.merge(list);
    CHECK_FALSE(cat.has_flag(11, MediaCatalog::kGone));
    CHECK_FALSE(cat.has_flag(11, MediaCatalog::kGoneReported));
    CHECK(cat.unreported_present(10).size() == 1);
}

TEST_CASE("the gone flags survive a save and load")
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11)};
    cat.merge(list);
    REQUIRE(cat.mark_gone(11));
    REQUIRE(cat.set_flag(11, MediaCatalog::kGoneReported));
    MediaCatalog loaded;
    REQUIRE(loaded.parse(cat.serialize()));
    CHECK(loaded.has_flag(11, MediaCatalog::kGone));
    CHECK(loaded.has_flag(11, MediaCatalog::kGoneReported));
}

TEST_CASE("the video duration is kept, v3 rows are read without it (#160)")
{
    MediaCatalog cat;
    MediaList list;
    MediaItem video = item(100, 7, 2, 555, 1000);
    video.duration_ms = 20000;
    list.files.push_back(video);
    list.files.push_back(item(100, 8, 0, 556, 1001));
    cat.merge(list);
    REQUIRE(cat.find(555) != nullptr);
    CHECK(cat.find(555)->duration_ms == 20000);
    CHECK(cat.find(556)->duration_ms == 0);

    // A later list without a duration keeps the known one.
    list.files[0].duration_ms = 0;
    cat.merge(list);
    CHECK(cat.find(555)->duration_ms == 20000);

    MediaCatalog loaded;
    REQUIRE(loaded.parse(cat.serialize()));
    CHECK(loaded.format_version() == 4);
    CHECK(loaded.find(555)->duration_ms == 20000);

    MediaCatalog old;
    REQUIRE(old.parse("# trailcamhub-catalog v3\n100,7,555,2,1000,3\n"));
    CHECK(old.format_version() == 3);
    REQUIRE(old.find(555) != nullptr);
    CHECK(old.find(555)->duration_ms == 0);
    CHECK(old.find(555)->flags == 3);
}
