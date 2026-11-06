#pragma once

// A tiny per-camera "database" of the media a camera holds and what we have
// done with each item: whether its thumbnail and its full file are stored,
// and (later) whether the user requested its video and whether that video was
// uploaded. It is kept in memory, merged with each fresh media list, and
// persisted as a small CSV file on the SD card. Pure logic, host-testable; the
// SD read/write lives in a separate runtime layer.
//
// An item is identified by its mediaId, not by dir/num: the camera reuses
// dir/num after its card was cleared, but never a mediaId (see
// docs/media-identity.md).

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
    // In the media list of the current camera session (set by merge(), not
    // persisted). Only these entries are on the camera's card right now, so
    // only these may be requested from the camera by dir/num.
    bool listed = false;
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
        // A requested video cannot be fetched: it is not on the camera's
        // card any more, or its download failed kMaxVideoAttempts times.
        kVideoUnavailable = 1u << 5,
    };
    // Failed downloads of a requested video, kept in two bits of the flags
    // (so the CSV format does not change); at this count it is given up.
    static constexpr std::uint32_t kMaxVideoAttempts = 3;

    // Which videos a camera session fetches.
    enum class VideoDownload {
        none,       // no videos
        missing,    // every video that is not on the SD card yet
        requested,  // only the videos the user asked for
    };

    // Adds entries from a freshly fetched list. Existing entries (matched by
    // mediaId) keep their flags; their dir/num/time/type are refreshed. A
    // known dir/num with a new mediaId is a new recording (the card was
    // cleared): it gets its own entry with no flags, and the old entry keeps
    // its flags. Items without a valid mediaId (missing or <= 0) are skipped.
    // Marks exactly the listed entries as `listed`. Returns the number of
    // newly added entries. Same as begin_listing() and merge_page() for a
    // list that fits in one page.
    std::size_t merge(const MediaList &list);

    // A media list read in pages: begin_listing() clears `listed` on every
    // entry, then merge_page() adds each page and marks its entries `listed`
    // without touching the others. After the last page exactly the entries of
    // all pages are `listed`.
    void begin_listing();
    std::size_t merge_page(const MediaList &page);

    CatalogEntry *find(std::int64_t media_id);
    const CatalogEntry *find(std::int64_t media_id) const;
    // The listed entry with this dir/num (unique on the card at one time), or
    // nullptr. For answers of the camera, which name dir/num.
    const CatalogEntry *find_listed(std::int64_t dir, std::int64_t num) const;

    bool set_flag(std::int64_t media_id, Flag flag);
    bool clear_flag(std::int64_t media_id, Flag flag);
    bool has_flag(std::int64_t media_id, Flag flag) const;

    // Work for the camera session, from the listed entries only: an entry
    // that is not on the card cannot be fetched, and its dir/num may belong
    // to another recording now. Each list is ordered newest first (by dir/num,
    // which is unique and increasing on the card), so that the newest
    // recordings are fetched first when a session cannot fetch them all.
    // Items whose thumbnail is not stored yet.
    std::vector<CatalogEntry> missing_thumbnails() const;
    // Video items the user requested that are not downloaded yet (and not
    // given up as unavailable).
    std::vector<CatalogEntry> pending_video_requests() const;
    // Items whose full file is not stored yet, photos before videos (each
    // newest first); each kind is included only if selected.
    std::vector<CatalogEntry> missing_downloads(bool photos, bool videos) const;
    // The downloads of a camera session: missing photos if `photos`, then the
    // videos `videos` selects, at most `max_videos` of them (0: no limit).
    std::vector<CatalogEntry> download_queue(bool photos, VideoDownload videos,
                                             std::size_t max_videos) const;

    // After a complete media list: requested videos that are not downloaded
    // and not on the camera's card are marked kVideoUnavailable. Returns how
    // many were newly marked.
    std::size_t mark_unlisted_requests_unavailable();
    // Failed downloads of a requested video so far.
    std::uint32_t video_attempts(std::int64_t media_id) const;
    // Counts a failed download of a requested video; at kMaxVideoAttempts it
    // is marked kVideoUnavailable. Returns the new count (0 if unknown).
    std::uint32_t count_failed_video_attempt(std::int64_t media_id);
    // Forgets a request (after it was answered as failed): clears
    // kVideoRequested, kVideoUnavailable and the attempts, so that a new
    // request starts over.
    bool forget_video_request(std::int64_t media_id);
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

    // CSV with a version header line (v3). parse() replaces the contents and
    // returns false if the header/version is not recognised. It also reads
    // v1 (keyed by dir/num) and v2 (files on the SD card still named by
    // dir/num), which have the same columns; rows without a valid mediaId and
    // repeated mediaIds are dropped.
    std::string serialize() const;
    bool parse(const std::string &text);
    // The version parse() read (1, 2 or 3); 0 for a new catalog. Below 3 the
    // files on the SD card are still named by dir/num.
    int format_version() const
    {
        return version_;
    }
    static constexpr int kFormatVersion = 3;
    void clear()
    {
        entries_.clear();
        version_ = 0;
    }

  private:
    std::vector<CatalogEntry> entries_;
    int version_ = 0;
};

}  // namespace camera::artemis
