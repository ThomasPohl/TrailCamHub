#pragma once

// A tiny per-camera "database" of the media a camera holds and what we have
// done with each item: whether its thumbnail and its full file are stored,
// and (later) whether the user requested its video and whether that video was
// uploaded. It is kept in memory, merged with each fresh media list, and
// persisted as a small CSV file on the SD card. Pure logic, host-testable; the
// SD read/write lives in a separate runtime layer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media_list.hpp"

namespace camera::artemis
{

struct CatalogEntry {
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
    std::int64_t media_id = 0;
    std::int64_t file_type = 0;  // 0 = photo, 2 = video (per the media list)
    std::int64_t media_time = 0;
    std::uint32_t flags = 0;
};

class MediaCatalog
{
  public:
    enum Flag : std::uint32_t {
        kThumbPresent = 1u << 0,  // the thumbnail JPEG is stored on the SD card
        kVideoRequested = 1u << 1,  // the user asked for this item's full video
        kFileDownloaded = 1u << 2,  // the full photo/video is on the SD card
        kVideoUploaded = 1u << 3,   // the full video was uploaded to the cloud
        kReported = 1u << 4,  // the backend knows this item (check-in accepted)
    };

    // Adds entries from a freshly fetched list. Existing entries (matched by
    // dir+num) keep their flags; their id/time/type are refreshed. Returns the
    // number of newly added entries.
    std::size_t merge(const MediaList &list);

    CatalogEntry *find(std::int64_t dir, std::int64_t num);
    const CatalogEntry *find(std::int64_t dir, std::int64_t num) const;

    bool set_flag(std::int64_t dir, std::int64_t num, Flag flag);
    bool clear_flag(std::int64_t dir, std::int64_t num, Flag flag);
    bool has_flag(std::int64_t dir, std::int64_t num, Flag flag) const;

    // Items whose thumbnail is not stored yet.
    std::vector<CatalogEntry> missing_thumbnails() const;
    // Video items the user requested that are not downloaded yet.
    std::vector<CatalogEntry> pending_video_requests() const;
    // Items whose full file is not stored yet, photos before videos; each
    // kind is included only if selected.
    std::vector<CatalogEntry> missing_downloads(bool photos, bool videos) const;
    // Up to `limit` items without `flag`, in catalog order.
    std::vector<CatalogEntry> without_flag(Flag flag, std::size_t limit) const;
    // Clears `flag` on every item (e.g. to report everything again).
    void clear_flag_everywhere(Flag flag);

    const std::vector<CatalogEntry> &entries() const
    {
        return entries_;
    }
    std::size_t size() const
    {
        return entries_.size();
    }
    std::size_t count_with_flag(Flag flag) const;

    // CSV with a version header line. parse() replaces the contents and
    // returns false if the header/version is not recognised.
    std::string serialize() const;
    bool parse(const std::string &text);
    void clear()
    {
        entries_.clear();
    }

  private:
    std::vector<CatalogEntry> entries_;
};

}  // namespace camera::artemis
