#include <doctest/doctest.h>

#include <string>

#include "media_catalog.hpp"
#include "media_list.hpp"
#include "media_list_flow.hpp"

using camera::link::ListingFlow;
using camera::link::ScanListFlow;
using camera::link::SdReport;

namespace
{

// A media-list answer with `files` items on page `page`.
std::string page_json(int page, int files, int first_num = 1)
{
    std::string json = "{\"cmdId\":768,\"result\":0,\"getMediaListRet\":0,"
                       "\"pageNo\":" +
                       std::to_string(page) +
                       ",\"cnt\":" + std::to_string(files) +
                       ",\"mediaFiles\":[";
    for (int i = 0; i < files; ++i) {
        const int num = first_num + i;
        json += std::string(i ? "," : "") +
                "{\"fileType\":" + std::to_string((num % 2) * 2) +
                ",\"dirNum\":100,\"mediaNum\":" + std::to_string(num) +
                ",\"mediaId\":" + std::to_string(1000 + num) +
                ",\"mediaTime\":" + std::to_string(1790000000 + num) + "}";
    }
    return json + "]}";
}

const int kFull = static_cast<int>(camera::artemis::kMediaListPageSize);

}  // namespace

TEST_CASE("media list fixture parses")
{
    camera::artemis::MediaList list;
    REQUIRE(camera::artemis::parse_media_list(page_json(0, 3), list));
    CHECK(list.files.size() == 3);
}

TEST_CASE("listing: pages until a short one, lines and SD report")
{
    ListingFlow flow;
    ListingFlow::Step step = flow.on_page(page_json(0, kFull));
    CHECK(step.next == ListingFlow::Next::request_page);
    CHECK(step.sd == SdReport::present);
    CHECK(step.lines.size() == static_cast<size_t>(kFull));
    CHECK(step.lines[0].rfind("media ", 0) == 0);
    CHECK(flow.page() == 1);

    step = flow.on_page(page_json(1, 2, kFull + 1));
    CHECK(step.next == ListingFlow::Next::done);
    CHECK(step.sd == SdReport::none);
    REQUIRE(step.lines.size() == 3);
    CHECK(step.lines.back() ==
          "list done files=" + std::to_string(kFull + 2) + " pages=2");
    CHECK(flow.files() == static_cast<uint32_t>(kFull + 2));
}

TEST_CASE("listing: truncated at the page limit, failures")
{
    ListingFlow limited(1);
    ListingFlow::Step step = limited.on_page(page_json(0, kFull));
    CHECK(step.next == ListingFlow::Next::done);
    CHECK(step.lines.back().rfind("list truncated", 0) == 0);

    ListingFlow flow;
    step = flow.on_page(
        "{\"cmdId\":768,\"getMediaListRet\":-5,\"errorMsg\":\"no sd\"}");
    CHECK(step.next == ListingFlow::Next::failed);
    CHECK(step.camera_error);
    CHECK(step.sd == SdReport::missing);
    CHECK(step.lines ==
          std::vector<std::string>{"list failed getMediaListRet=-5 no sd"});

    flow.reset();
    step = flow.on_page("{\"cmdId\":768}");
    CHECK(step.next == ListingFlow::Next::failed);
    CHECK(step.lines.back() == "list failed no mediaFiles array");

    flow.reset();
    step = flow.on_page(page_json(3, 1));
    CHECK(step.next == ListingFlow::Next::failed);
    CHECK(step.lines.back() == "list failed asked for page 0, got 3");
}

TEST_CASE("scan list: merges pages, counts, completes")
{
    camera::artemis::MediaCatalog catalog;
    catalog.begin_listing();
    ScanListFlow flow(20);
    ScanListFlow::Step step = flow.on_page(page_json(0, kFull), catalog);
    CHECK(step.next == ScanListFlow::Next::request_page);
    CHECK(step.sd == SdReport::present);
    CHECK(step.added == static_cast<size_t>(kFull));

    // A late repeat of page 0 is ignored.
    step = flow.on_page(page_json(0, kFull), catalog);
    CHECK(step.next == ScanListFlow::Next::ignore);
    CHECK(flow.page() == 1);

    step = flow.on_page(page_json(1, 3, kFull + 1), catalog);
    CHECK(step.next == ScanListFlow::Next::complete);
    CHECK(flow.added() == static_cast<uint32_t>(kFull + 3));
    CHECK(flow.photos() + flow.videos() == static_cast<uint32_t>(kFull + 3));
    CHECK(catalog.size() == static_cast<size_t>(kFull + 3));
}

TEST_CASE("scan list: page limit and failures")
{
    camera::artemis::MediaCatalog catalog;
    catalog.begin_listing();
    ScanListFlow limited(1);
    ScanListFlow::Step step = limited.on_page(page_json(0, kFull), catalog);
    CHECK(step.next == ScanListFlow::Next::partial);
    CHECK(step.page_limit);

    ScanListFlow flow(20);
    step = flow.on_page("{\"cmdId\":768,\"getMediaListRet\":-5}", catalog);
    CHECK(step.next == ScanListFlow::Next::end_session);
    CHECK(step.camera_error);
    CHECK(step.sd == SdReport::missing);

    flow.reset();
    step = flow.on_page("{\"cmdId\":768}", catalog);
    CHECK(step.next == ScanListFlow::Next::end_session);
    CHECK(step.unparsed);
    CHECK(step.sd == SdReport::none);

    flow.reset();
    REQUIRE(flow.on_page(page_json(0, kFull), catalog).next ==
            ScanListFlow::Next::request_page);
    step = flow.on_page("{\"cmdId\":768}", catalog);
    CHECK(step.next == ScanListFlow::Next::partial);
    CHECK_FALSE(step.page_limit);
}
