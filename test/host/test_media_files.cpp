#include <doctest/doctest.h>

#include "media_catalog.hpp"
#include "media_files.hpp"
#include "media_list.hpp"

using camera::artemis::MediaCatalog;
using camera::artemis::MediaItem;
using camera::artemis::MediaList;
using namespace camera::store;

namespace
{
MediaItem item(std::int64_t dir, std::int64_t num, std::int64_t id)
{
    MediaItem m;
    m.dir_num = dir;
    m.num = num;
    m.id = id;
    return m;
}
}  // namespace

TEST_CASE("files are named by the mediaId")
{
    CHECK(media_base_name(4136037234) == "4136037234");
}

TEST_CASE("parse_legacy_name reads <dir>_<num>.<rest>")
{
    std::int64_t dir = 0;
    std::int64_t num = 0;
    std::string rest;
    REQUIRE(parse_legacy_name("100_36.jpg", dir, num, rest));
    CHECK(dir == 100);
    CHECK(num == 36);
    CHECK(rest == "jpg");
    REQUIRE(parse_legacy_name("100_2.h264.part", dir, num, rest));
    CHECK(num == 2);
    CHECK(rest == "h264.part");

    CHECK_FALSE(parse_legacy_name("4136037234.jpg", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("100_36", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("100_36.", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("_36.jpg", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("100_.jpg", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("a100_36.jpg", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("100-36.jpg", dir, num, rest));
    CHECK_FALSE(parse_legacy_name("1000000000000_1.jpg", dir, num, rest));
}

TEST_CASE("plan_legacy_files gives unique dir/num to their entry")
{
    MediaCatalog catalog;
    MediaList list;
    list.files = {item(100, 1, 111), item(100, 2, 222)};
    catalog.merge(list);

    const LegacyFilePlan plan = plan_legacy_files(catalog);
    CHECK(plan.owner.size() == 2);
    CHECK(plan.owner.at({100, 1}) == 111);
    CHECK(plan.owner.at({100, 2}) == 222);
    CHECK(plan.ambiguous.empty());
    CHECK(plan.ambiguous_media_ids.empty());
}

TEST_CASE("plan_legacy_files drops a dir/num shared after a card clear")
{
    MediaCatalog catalog;
    MediaList before;
    before.files = {item(100, 1, 111), item(100, 2, 222)};
    catalog.merge(before);
    MediaList after;
    after.files = {item(100, 1, 999)};
    catalog.merge(after);

    const LegacyFilePlan plan = plan_legacy_files(catalog);
    CHECK(plan.owner.size() == 1);
    CHECK(plan.owner.at({100, 2}) == 222);
    REQUIRE(plan.ambiguous.size() == 1);
    CHECK(plan.ambiguous[0] ==
          std::make_pair<std::int64_t, std::int64_t>(100, 1));
    CHECK(plan.ambiguous_media_ids == std::vector<std::int64_t>{111, 999});
}

TEST_CASE("a file belongs to a recording by its whole base name (#461)")
{
    CHECK(is_file_of_media("2035.jpg", 2035));
    CHECK(is_file_of_media("2035.mp4", 2035));
    CHECK(is_file_of_media("2035.mp4.part", 2035));
    CHECK(is_file_of_media("2035.part", 2035));
    // Another recording whose number starts the same, or a name without the
    // dot.
    CHECK_FALSE(is_file_of_media("20350.jpg", 2035));
    CHECK_FALSE(is_file_of_media("203.jpg", 2035));
    CHECK_FALSE(is_file_of_media("2035", 2035));
    CHECK_FALSE(is_file_of_media("2035.", 2035));
    CHECK_FALSE(is_file_of_media("x2035.jpg", 2035));
    CHECK_FALSE(is_file_of_media("100_2.jpg", 2035));
}
