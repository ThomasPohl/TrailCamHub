#include "media_list_flow.hpp"

namespace camera::link
{

using artemis::format_media_item;
using artemis::media_list_failed;
using artemis::media_list_has_more;
using artemis::MediaItem;
using artemis::parse_media_list;

void ListingFlow::reset()
{
    page_ = 0;
    files_ = 0;
}

ListingFlow::Step ListingFlow::on_page(const std::string &json)
{
    Step step;
    const bool parsed = parse_media_list(json, step.list);
    auto fail = [&step](const std::string &reason) {
        step.next = Next::failed;
        step.lines.push_back("list failed " + reason);
    };
    if (media_list_failed(step.list)) {
        // Also without a mediaFiles array: the camera named the error.
        step.camera_error = true;
        if (page_ == 0) {
            step.sd = SdReport::missing;
        }
        fail("getMediaListRet=" + std::to_string(step.list.list_ret) +
             (step.list.error_msg.empty() ? "" : " " + step.list.error_msg));
        return step;
    }
    if (!parsed) {
        fail("no mediaFiles array");
        return step;
    }
    if (page_ == 0) {
        step.sd = SdReport::present;
    }
    if (step.list.page_no != page_) {
        fail("asked for page " + std::to_string(page_) + ", got " +
             std::to_string(step.list.page_no));
        return step;
    }
    for (const MediaItem &item : step.list.files) {
        step.lines.push_back("media " + format_media_item(item));
        ++files_;
    }
    ++page_;
    const std::string counts =
        " files=" + std::to_string(files_) + " pages=" + std::to_string(page_);
    if (!media_list_has_more(step.list)) {
        step.next = Next::done;
        step.lines.push_back("list done" + counts);
    } else if (page_ >= max_pages_) {
        step.next = Next::done;
        step.lines.push_back("list truncated" + counts);
    } else {
        step.next = Next::request_page;
    }
    return step;
}

void ScanListFlow::reset()
{
    page_ = 0;
    photos_ = 0;
    videos_ = 0;
    added_ = 0;
}

ScanListFlow::Step ScanListFlow::on_page(const std::string &json,
                                         artemis::MediaCatalog &catalog)
{
    Step step;
    const bool parsed = parse_media_list(json, step.list);
    step.unparsed = !parsed;
    if (!parsed || media_list_failed(step.list)) {
        step.camera_error = media_list_failed(step.list);
        if (page_ == 0) {
            // Nothing to fetch: no thumbnail or download would be queued.
            // The catalog is left as it is.
            step.next = Next::end_session;
            if (step.camera_error) {
                step.sd = SdReport::missing;
            }
        } else {
            // Work with the pages read so far.
            step.next = Next::partial;
        }
        return step;
    }
    if (step.list.page_no != page_) {
        // A late answer to an earlier request; the answer to this one follows.
        step.next = Next::ignore;
        return step;
    }
    if (page_ == 0) {
        step.sd = SdReport::present;
    }
    for (const MediaItem &item : step.list.files) {
        if (item.file_type == 0) {
            ++photos_;
        } else {
            ++videos_;
        }
    }
    step.added = catalog.merge_page(step.list);
    added_ += static_cast<std::uint32_t>(step.added);
    ++page_;
    if (!media_list_has_more(step.list)) {
        step.next = Next::complete;
    } else if (page_ >= max_pages_) {
        step.next = Next::partial;
        step.page_limit = true;
    } else {
        step.next = Next::request_page;
    }
    return step;
}

}  // namespace camera::link
