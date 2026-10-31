#include "media_catalog.hpp"

#include <cstdlib>

namespace camera::artemis
{

namespace
{
constexpr char kHeader[] = "# trailcamhub-catalog v1";
}

std::size_t MediaCatalog::merge(const MediaList &list)
{
    std::size_t added = 0;
    for (const MediaItem &item : list.files) {
        CatalogEntry *e = find(item.dir_num, item.num);
        if (e != nullptr) {
            e->media_id = item.id;
            e->media_time = item.time;
            e->file_type = item.file_type;
            continue;
        }
        CatalogEntry n;
        n.dir_num = item.dir_num;
        n.media_num = item.num;
        n.media_id = item.id;
        n.file_type = item.file_type;
        n.media_time = item.time;
        n.flags = 0;
        entries_.push_back(n);
        ++added;
    }
    return added;
}

CatalogEntry *MediaCatalog::find(std::int64_t dir, std::int64_t num)
{
    for (CatalogEntry &e : entries_) {
        if (e.dir_num == dir && e.media_num == num)
            return &e;
    }
    return nullptr;
}

const CatalogEntry *MediaCatalog::find(std::int64_t dir, std::int64_t num) const
{
    for (const CatalogEntry &e : entries_) {
        if (e.dir_num == dir && e.media_num == num)
            return &e;
    }
    return nullptr;
}

bool MediaCatalog::set_flag(std::int64_t dir, std::int64_t num, Flag flag)
{
    CatalogEntry *e = find(dir, num);
    if (e == nullptr)
        return false;
    e->flags |= static_cast<std::uint32_t>(flag);
    return true;
}

bool MediaCatalog::clear_flag(std::int64_t dir, std::int64_t num, Flag flag)
{
    CatalogEntry *e = find(dir, num);
    if (e == nullptr)
        return false;
    e->flags &= ~static_cast<std::uint32_t>(flag);
    return true;
}

bool MediaCatalog::has_flag(std::int64_t dir, std::int64_t num, Flag flag) const
{
    const CatalogEntry *e = find(dir, num);
    return e != nullptr && (e->flags & static_cast<std::uint32_t>(flag)) != 0;
}

std::vector<CatalogEntry> MediaCatalog::missing_thumbnails() const
{
    std::vector<CatalogEntry> out;
    for (const CatalogEntry &e : entries_) {
        if ((e.flags & kThumbPresent) == 0)
            out.push_back(e);
    }
    return out;
}

std::vector<CatalogEntry> MediaCatalog::pending_video_requests() const
{
    std::vector<CatalogEntry> out;
    for (const CatalogEntry &e : entries_) {
        if ((e.flags & kVideoRequested) != 0 &&
            (e.flags & kFileDownloaded) == 0)
            out.push_back(e);
    }
    return out;
}

std::vector<CatalogEntry> MediaCatalog::missing_downloads(bool photos,
                                                          bool videos) const
{
    std::vector<CatalogEntry> out;
    for (bool want_photos : {true, false}) {
        if (want_photos ? !photos : !videos)
            continue;
        for (const CatalogEntry &e : entries_) {
            const bool is_photo = e.file_type == 0;
            if (is_photo == want_photos && (e.flags & kFileDownloaded) == 0)
                out.push_back(e);
        }
    }
    return out;
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
                header_seen = true;
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
