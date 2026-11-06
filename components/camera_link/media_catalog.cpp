#include "media_catalog.hpp"

#include <algorithm>
#include <cstdlib>

namespace camera::artemis
{

namespace
{
// v3: keyed by mediaId, files on the SD card named by mediaId.
constexpr char kHeader[] = "# trailcamhub-catalog v3";
// v2: keyed by mediaId, files still named by dir/num.
constexpr char kHeaderV2[] = "# trailcamhub-catalog v2";
// v1: keyed by dir/num (before #85). All versions have the same columns.
constexpr char kHeaderV1[] = "# trailcamhub-catalog v1";

bool valid_media_id(std::int64_t id)
{
    return id > 0;
}

constexpr std::uint32_t kAttemptShift = 6;
constexpr std::uint32_t kAttemptMask = 3u << kAttemptShift;

// Newest first: a higher dir/num is a later recording on the card.
void sort_newest_first(std::vector<CatalogEntry> &entries)
{
    std::stable_sort(entries.begin(), entries.end(),
                     [](const CatalogEntry &a, const CatalogEntry &b) {
                         if (a.dir_num != b.dir_num)
                             return a.dir_num > b.dir_num;
                         return a.media_num > b.media_num;
                     });
}
}  // namespace

std::size_t MediaCatalog::merge(const MediaList &list)
{
    begin_listing();
    return merge_page(list);
}

void MediaCatalog::begin_listing()
{
    for (CatalogEntry &e : entries_)
        e.listed = false;
}

std::size_t MediaCatalog::merge_page(const MediaList &page)
{
    std::size_t added = 0;
    for (const MediaItem &item : page.files) {
        if (!valid_media_id(item.id))
            continue;
        CatalogEntry *e = find(item.id);
        if (e != nullptr) {
            e->dir_num = item.dir_num;
            e->media_num = item.num;
            e->media_time = item.time;
            e->file_type = item.file_type;
            e->listed = true;
            continue;
        }
        CatalogEntry n;
        n.dir_num = item.dir_num;
        n.media_num = item.num;
        n.media_id = item.id;
        n.file_type = item.file_type;
        n.media_time = item.time;
        n.flags = 0;
        n.listed = true;
        entries_.push_back(n);
        ++added;
    }
    return added;
}

CatalogEntry *MediaCatalog::find(std::int64_t media_id)
{
    for (CatalogEntry &e : entries_) {
        if (e.media_id == media_id)
            return &e;
    }
    return nullptr;
}

const CatalogEntry *MediaCatalog::find(std::int64_t media_id) const
{
    for (const CatalogEntry &e : entries_) {
        if (e.media_id == media_id)
            return &e;
    }
    return nullptr;
}

const CatalogEntry *MediaCatalog::find_listed(std::int64_t dir,
                                              std::int64_t num) const
{
    for (const CatalogEntry &e : entries_) {
        if (e.listed && e.dir_num == dir && e.media_num == num)
            return &e;
    }
    return nullptr;
}

bool MediaCatalog::set_flag(std::int64_t media_id, Flag flag)
{
    CatalogEntry *e = find(media_id);
    if (e == nullptr)
        return false;
    e->flags |= static_cast<std::uint32_t>(flag);
    return true;
}

bool MediaCatalog::clear_flag(std::int64_t media_id, Flag flag)
{
    CatalogEntry *e = find(media_id);
    if (e == nullptr)
        return false;
    e->flags &= ~static_cast<std::uint32_t>(flag);
    return true;
}

bool MediaCatalog::has_flag(std::int64_t media_id, Flag flag) const
{
    const CatalogEntry *e = find(media_id);
    return e != nullptr && (e->flags & static_cast<std::uint32_t>(flag)) != 0;
}

std::vector<CatalogEntry> MediaCatalog::missing_thumbnails() const
{
    std::vector<CatalogEntry> out;
    for (const CatalogEntry &e : entries_) {
        if (e.listed && (e.flags & kThumbPresent) == 0)
            out.push_back(e);
    }
    sort_newest_first(out);
    return out;
}

std::vector<CatalogEntry> MediaCatalog::pending_video_requests() const
{
    std::vector<CatalogEntry> out;
    for (const CatalogEntry &e : entries_) {
        if (e.listed && (e.flags & kVideoRequested) != 0 &&
            (e.flags & (kFileDownloaded | kVideoUnavailable)) == 0)
            out.push_back(e);
    }
    sort_newest_first(out);
    return out;
}

std::vector<CatalogEntry> MediaCatalog::missing_downloads(bool photos,
                                                          bool videos) const
{
    std::vector<CatalogEntry> out;
    for (bool want_photos : {true, false}) {
        if (want_photos ? !photos : !videos)
            continue;
        std::vector<CatalogEntry> kind;
        for (const CatalogEntry &e : entries_) {
            const bool is_photo = e.file_type == 0;
            if (e.listed && is_photo == want_photos &&
                (e.flags & kFileDownloaded) == 0)
                kind.push_back(e);
        }
        sort_newest_first(kind);
        out.insert(out.end(), kind.begin(), kind.end());
    }
    return out;
}

std::vector<CatalogEntry>
MediaCatalog::download_queue(bool photos, VideoDownload videos,
                             std::size_t max_videos) const
{
    std::vector<CatalogEntry> out = missing_downloads(photos, false);
    std::vector<CatalogEntry> wanted;
    switch (videos) {
    case VideoDownload::none:
        break;
    case VideoDownload::missing:
        wanted = missing_downloads(false, true);
        break;
    case VideoDownload::requested:
        wanted = pending_video_requests();
        break;
    }
    if (max_videos > 0 && wanted.size() > max_videos)
        wanted.resize(max_videos);
    out.insert(out.end(), wanted.begin(), wanted.end());
    return out;
}

std::size_t MediaCatalog::mark_unlisted_requests_unavailable()
{
    std::size_t marked = 0;
    for (CatalogEntry &e : entries_) {
        if (!e.listed && (e.flags & kVideoRequested) != 0 &&
            (e.flags & (kFileDownloaded | kVideoUnavailable)) == 0) {
            e.flags |= kVideoUnavailable;
            ++marked;
        }
    }
    return marked;
}

std::uint32_t MediaCatalog::video_attempts(std::int64_t media_id) const
{
    const CatalogEntry *e = find(media_id);
    return e == nullptr ? 0 : (e->flags & kAttemptMask) >> kAttemptShift;
}

std::uint32_t MediaCatalog::count_failed_video_attempt(std::int64_t media_id)
{
    CatalogEntry *e = find(media_id);
    if (e == nullptr)
        return 0;
    std::uint32_t attempts = (e->flags & kAttemptMask) >> kAttemptShift;
    if (attempts < kMaxVideoAttempts)
        ++attempts;
    e->flags = (e->flags & ~kAttemptMask) | (attempts << kAttemptShift);
    if (attempts >= kMaxVideoAttempts)
        e->flags |= kVideoUnavailable;
    return attempts;
}

bool MediaCatalog::forget_video_request(std::int64_t media_id)
{
    CatalogEntry *e = find(media_id);
    if (e == nullptr)
        return false;
    const std::uint32_t before = e->flags;
    e->flags &= ~(static_cast<std::uint32_t>(kVideoRequested) |
                  static_cast<std::uint32_t>(kVideoUnavailable) | kAttemptMask);
    return e->flags != before;
}

std::vector<CatalogEntry> MediaCatalog::without_flag(Flag flag,
                                                     std::size_t limit) const
{
    std::vector<CatalogEntry> out;
    for (const CatalogEntry &e : entries_) {
        if (out.size() >= limit)
            break;
        if ((e.flags & static_cast<std::uint32_t>(flag)) == 0)
            out.push_back(e);
    }
    return out;
}

void MediaCatalog::clear_flag_everywhere(Flag flag)
{
    for (CatalogEntry &e : entries_)
        e.flags &= ~static_cast<std::uint32_t>(flag);
}

std::size_t MediaCatalog::count_with_flag(Flag flag) const
{
    std::size_t n = 0;
    for (const CatalogEntry &e : entries_) {
        if ((e.flags & static_cast<std::uint32_t>(flag)) != 0)
            ++n;
    }
    return n;
}

std::string MediaCatalog::serialize() const
{
    std::string out = kHeader;
    out += '\n';
    for (const CatalogEntry &e : entries_) {
        out += std::to_string(e.dir_num);
        out += ',';
        out += std::to_string(e.media_num);
        out += ',';
        out += std::to_string(e.media_id);
        out += ',';
        out += std::to_string(e.file_type);
        out += ',';
        out += std::to_string(e.media_time);
        out += ',';
        out += std::to_string(e.flags);
        out += '\n';
    }
    return out;
}

bool MediaCatalog::parse(const std::string &text)
{
    entries_.clear();
    version_ = 0;
    std::size_t pos = 0;
    bool header_seen = false;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line[0] == '#') {
            if (line == kHeader)
                version_ = 3;
            else if (line == kHeaderV2)
                version_ = 2;
            else if (line == kHeaderV1)
                version_ = 1;
            header_seen = header_seen || version_ != 0;
            continue;
        }

        // Parse exactly six comma-separated integer fields.
        std::int64_t field[6];
        int fi = 0;
        std::size_t fp = 0;
        bool ok = true;
        while (fi < 6) {
            std::size_t comma = line.find(',', fp);
            std::string tok =
                line.substr(fp, comma == std::string::npos ? std::string::npos
                                                           : comma - fp);
            if (tok.empty()) {
                ok = false;
                break;
            }
            char *end = nullptr;
            field[fi] = std::strtoll(tok.c_str(), &end, 10);
            if (end == tok.c_str() || *end != '\0') {
                ok = false;
                break;
            }
            ++fi;
            if (comma == std::string::npos)
                break;
            fp = comma + 1;
        }
        if (!ok || fi != 6)
            continue;  // skip malformed rows rather than failing the load
        // Without a valid mediaId a row cannot be addressed; a repeated one
        // can only come from a damaged file. Keep the first.
        if (!valid_media_id(field[2]) || find(field[2]) != nullptr)
            continue;

        CatalogEntry e;
        e.dir_num = field[0];
        e.media_num = field[1];
        e.media_id = field[2];
        e.file_type = field[3];
        e.media_time = field[4];
        e.flags = static_cast<std::uint32_t>(field[5]);
        entries_.push_back(e);
    }
    return header_seen;
}

}  // namespace camera::artemis
