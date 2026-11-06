#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "media_catalog.hpp"
#include "mp4_file_source.hpp"
#include "video_request.hpp"

using camera::artemis::MediaCatalog;
using camera::artemis::MediaItem;
using camera::artemis::MediaList;
using uplink::decide_video;
using uplink::VideoFailure;
using uplink::VideoStep;

namespace
{

MediaItem item(std::int64_t dir, std::int64_t num, std::int64_t type,
               std::int64_t id)
{
    MediaItem m;
    m.dir_num = dir;
    m.num = num;
    m.file_type = type;
    m.id = id;
    m.time = 1766505692 + num;
    return m;
}

// Photo 11, videos 22, 33, 44 (newest last), all on the card.
MediaCatalog catalog()
{
    MediaCatalog cat;
    MediaList list;
    list.files = {item(100, 1, 0, 11), item(100, 2, 2, 22), item(100, 3, 2, 33),
                  item(100, 4, 2, 44)};
    cat.merge(list);
    return cat;
}

}  // namespace

TEST_CASE("download_queue: photos, then the selected videos")
{
    MediaCatalog cat = catalog();
    cat.set_flag(22, MediaCatalog::kVideoRequested);
    using VD = MediaCatalog::VideoDownload;

    SUBCASE("no videos")
    {
        const auto queue = cat.download_queue(true, VD::none, 0);
        REQUIRE(queue.size() == 1);
        CHECK(queue[0].media_id == 11);
    }
    SUBCASE("every missing video, newest first, limited")
    {
        const auto queue = cat.download_queue(false, VD::missing, 2);
        REQUIRE(queue.size() == 2);
        CHECK(queue[0].media_id == 44);
        CHECK(queue[1].media_id == 33);
    }
    SUBCASE("only requested videos")
    {
        const auto queue = cat.download_queue(true, VD::requested, 3);
        REQUIRE(queue.size() == 2);
        CHECK(queue[0].media_id == 11);
        CHECK(queue[1].media_id == 22);
    }
    SUBCASE("an unavailable request is not fetched")
    {
        cat.set_flag(22, MediaCatalog::kVideoUnavailable);
        CHECK(cat.download_queue(false, VD::requested, 0).empty());
    }
}

TEST_CASE("requested videos not on the card become unavailable")
{
    MediaCatalog cat = catalog();
    cat.set_flag(22, MediaCatalog::kVideoRequested);
    cat.set_flag(33, MediaCatalog::kVideoRequested);
    cat.set_flag(33, MediaCatalog::kFileDownloaded);
    // The next listing has only 33 and 44: 22 was deleted on the camera.
    MediaList list;
    list.files = {item(100, 3, 2, 33), item(100, 4, 2, 44)};
    cat.merge(list);

    CHECK(cat.mark_unlisted_requests_unavailable() == 1);
    CHECK(cat.has_flag(22, MediaCatalog::kVideoUnavailable));
    // Downloaded already: it can still be uploaded.
    CHECK_FALSE(cat.has_flag(33, MediaCatalog::kVideoUnavailable));
    // Unrequested ones are left alone.
    CHECK_FALSE(cat.has_flag(11, MediaCatalog::kVideoUnavailable));
    // Marked once.
    CHECK(cat.mark_unlisted_requests_unavailable() == 0);
}

TEST_CASE("failed video downloads are counted in the flags")
{
    MediaCatalog cat = catalog();
    cat.set_flag(22, MediaCatalog::kVideoRequested);
    CHECK(cat.video_attempts(22) == 0);
    CHECK(cat.count_failed_video_attempt(22) == 1);
    CHECK(cat.count_failed_video_attempt(22) == 2);
    CHECK_FALSE(cat.has_flag(22, MediaCatalog::kVideoUnavailable));
    CHECK(cat.count_failed_video_attempt(22) == 3);
    CHECK(cat.has_flag(22, MediaCatalog::kVideoUnavailable));
    CHECK(cat.count_failed_video_attempt(22) == 3);  // stays at the limit
    CHECK(cat.count_failed_video_attempt(99) == 0);  // unknown

    SUBCASE("kept in the CSV")
    {
        MediaCatalog loaded;
        REQUIRE(loaded.parse(cat.serialize()));
        CHECK(loaded.video_attempts(22) == 3);
        CHECK(loaded.has_flag(22, MediaCatalog::kVideoUnavailable));
        CHECK(loaded.has_flag(22, MediaCatalog::kVideoRequested));
    }
    SUBCASE("forgetting the request starts over")
    {
        cat.set_flag(22, MediaCatalog::kThumbPresent);
        CHECK(cat.forget_video_request(22));
        CHECK(cat.video_attempts(22) == 0);
        CHECK_FALSE(cat.has_flag(22, MediaCatalog::kVideoRequested));
        CHECK_FALSE(cat.has_flag(22, MediaCatalog::kVideoUnavailable));
        CHECK(cat.has_flag(22, MediaCatalog::kThumbPresent));
        CHECK_FALSE(cat.forget_video_request(22));  // nothing to clear
    }
}

TEST_CASE("decide_video: what to do with a requestVideo command")
{
    MediaCatalog cat = catalog();

    SUBCASE("unknown camera or recording")
    {
        auto d = decide_video(nullptr, 22, true, true);
        CHECK(d.step == VideoStep::fail);
        CHECK(d.failure == VideoFailure::not_on_hub);
        d = decide_video(&cat, 99, true, true);
        CHECK(d.failure == VideoFailure::not_on_hub);
    }
    SUBCASE("a photo")
    {
        CHECK(decide_video(&cat, 11, true, true).failure ==
              VideoFailure::not_a_video);
    }
    SUBCASE("new request: flag it and read the camera")
    {
        CHECK(decide_video(&cat, 22, true, true).step == VideoStep::request);
        cat.set_flag(22, MediaCatalog::kVideoRequested);
        CHECK(decide_video(&cat, 22, true, true).step == VideoStep::wait);
    }
    SUBCASE("a build without video downloads")
    {
        CHECK(decide_video(&cat, 22, false, true).failure ==
              VideoFailure::unsupported);
    }
    SUBCASE("downloaded: upload, also without downloads in this build")
    {
        cat.set_flag(22, MediaCatalog::kFileDownloaded);
        CHECK(decide_video(&cat, 22, true, true).step == VideoStep::upload);
        CHECK(decide_video(&cat, 22, false, true).step == VideoStep::upload);
        CHECK(decide_video(&cat, 22, true, false).failure ==
              VideoFailure::no_url);
    }
    SUBCASE("uploaded before")
    {
        cat.set_flag(22, MediaCatalog::kFileDownloaded);
        cat.set_flag(22, MediaCatalog::kVideoUploaded);
        CHECK(decide_video(&cat, 22, true, false).step == VideoStep::done);
    }
    SUBCASE("not on the camera any more")
    {
        cat.set_flag(22, MediaCatalog::kVideoRequested);
        cat.set_flag(22, MediaCatalog::kVideoUnavailable);
        CHECK(decide_video(&cat, 22, true, true).failure ==
              VideoFailure::not_on_camera);
    }
    SUBCASE("given up after failed downloads")
    {
        cat.set_flag(22, MediaCatalog::kVideoRequested);
        for (std::uint32_t i = 0; i < MediaCatalog::kMaxVideoAttempts; ++i) {
            cat.count_failed_video_attempt(22);
        }
        const auto d = decide_video(&cat, 22, true, true);
        CHECK(d.step == VideoStep::fail);
        CHECK(d.failure == VideoFailure::gave_up);
        CHECK(std::string(uplink::video_failure_text(d.failure))
                  .find("three times") != std::string::npos);
    }
}

TEST_CASE("mp4::FileSource reads a file at any offset")
{
    const std::string path = "test_file_source.bin";
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        for (int i = 0; i < 100; ++i) {
            std::fputc(i, f);
        }
        std::fclose(f);
    }
    mp4::FileSource source(path);
    REQUIRE(source.exists());
    CHECK(source.size() == 100);
    std::uint8_t buffer[10] = {};
    REQUIRE(source.read(0, buffer, 4));
    CHECK(buffer[3] == 3);
    REQUIRE(source.read(4, buffer, 2));  // sequential
    CHECK(buffer[0] == 4);
    REQUIRE(source.read(90, buffer, 10));  // seek
    CHECK(buffer[9] == 99);
    CHECK_FALSE(source.read(95, buffer, 10));  // past the end
    source.release();
    REQUIRE(source.read(50, buffer, 1));  // opens again
    CHECK(buffer[0] == 50);
    std::remove(path.c_str());

    mp4::FileSource missing("does-not-exist.bin");
    CHECK_FALSE(missing.exists());
    CHECK_FALSE(missing.read(0, buffer, 1));
}
