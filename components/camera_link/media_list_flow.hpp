#pragma once

// The page-by-page logic of reading a camera's media list (cmdId 768), out of
// camera_protocol.cpp (#65): which page to ask for next, when the list is
// complete, what to report, and what the first page says about the camera's
// SD card. Pure logic (no ESP-IDF), host-tested; camera_protocol.cpp sends
// the requests, logs and records.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media_catalog.hpp"
#include "media_list.hpp"

namespace camera::link
{

// What the first page said about the camera's SD card.
enum class SdReport : std::uint8_t {
    none,     // not the first page, or nothing to say
    present,  // the camera listed its card
    missing,  // the camera named an error (no card, card error)
};

// "camlist": every file as a line, for the console and the installer.
class ListingFlow
{
  public:
    enum class Next : std::uint8_t {
        request_page,  // ask for page()
        done,          // the last line ("list done|truncated ...") is in lines
        failed,        // "list failed <reason>" is in lines
    };

    struct Step {
        Next next = Next::failed;
        // Lines to report in order: "media <entry>" per file, then the final
        // line if the listing ended.
        std::vector<std::string> lines;
        SdReport sd = SdReport::none;
        // The camera named an error (getMediaListRet != 0).
        bool camera_error = false;
        // The parsed page (for recording the SD card state).
        camera::artemis::MediaList list;
    };

    explicit ListingFlow(std::int64_t max_pages = artemis::kMediaListMaxPages)
        : max_pages_(max_pages)
    {
    }

    // Starts over at page 0.
    void reset();
    // Handles the answer to the request for page().
    Step on_page(const std::string &json);

    // The page to ask for next.
    std::int64_t page() const
    {
        return page_;
    }
    std::uint32_t files() const
    {
        return files_;
    }

  private:
    std::int64_t max_pages_;
    std::int64_t page_ = 0;
    std::uint32_t files_ = 0;
};

// A scan: each page is merged into the catalog on its own, so only one page
// is held in RAM. The camera reports no total; a short or empty page ends
// the list.
class ScanListFlow
{
  public:
    enum class Next : std::uint8_t {
        request_page,  // ask for page()
        ignore,        // a late answer to an earlier request; keep waiting
        complete,      // every page was read: unlisted items are gone
        partial,       // stop here (page limit or a failed later page)
        end_session,   // the first page failed: nothing to fetch
    };

    struct Step {
        Next next = Next::end_session;
        SdReport sd = SdReport::none;
        // The camera named an error (getMediaListRet != 0).
        bool camera_error = false;
        // The page had no mediaFiles array.
        bool unparsed = false;
        // New catalog entries from this page.
        std::size_t added = 0;
        // The page hit the configured page limit.
        bool page_limit = false;
        camera::artemis::MediaList list;
    };

    explicit ScanListFlow(std::int64_t max_pages) : max_pages_(max_pages)
    {
    }

    void reset();
    // Handles the answer to the request for page() and merges it into
    // `catalog` (after begin_listing()).
    Step on_page(const std::string &json, artemis::MediaCatalog &catalog);

    std::int64_t page() const
    {
        return page_;
    }
    std::uint32_t photos() const
    {
        return photos_;
    }
    std::uint32_t videos() const
    {
        return videos_;
    }
    std::uint32_t added() const
    {
        return added_;
    }

  private:
    std::int64_t max_pages_;
    std::int64_t page_ = 0;
    std::uint32_t photos_ = 0;
    std::uint32_t videos_ = 0;
    std::uint32_t added_ = 0;
};

}  // namespace camera::link
