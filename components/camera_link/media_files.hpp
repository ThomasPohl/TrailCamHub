#pragma once

// File names of a camera's media on the hub's SD card, and the plan to move
// files from the old names to the new ones. Pure logic, host-testable; the
// file operations live in media_store.cpp.
//
// A recording's files are named by its mediaId (docs/media-identity.md):
//     thumb/<mediaId>.jpg
//     media/<mediaId>.<jpg|mp4|mov|avi|bin|h264|aac>
// Before catalog v3 they were named <dir>_<num>, which the camera reuses after
// its card was cleared.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "media_catalog.hpp"

namespace camera::store
{

// Base name (without extension) of a recording's files.
std::string media_base_name(std::int64_t media_id);

// A file name in thumb/ or media/ that belongs to this recording: its base
// name, a dot and anything after it ("<mediaId>.jpg", "<mediaId>.mp4.part").
// "12.jpg" is not a file of 123, and a name without the dot is none either.
bool is_file_of_media(const std::string &name, std::int64_t media_id);

// Splits an old file name "<dir>_<num>.<rest>" (rest is everything after the
// first '.', e.g. "jpg" or "h264.part"). False for any other name.
bool parse_legacy_name(const std::string &name, std::int64_t &dir,
                       std::int64_t &num, std::string &rest);

// What to do with files named by dir/num, from the catalog that was stored
// with them.
struct LegacyFilePlan {
    // dir/num owned by exactly one catalog entry: its files get its mediaId.
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> owner;
    // dir/num shared by several entries (a card clear under firmware that
    // already kept both): the file belongs to one of them, which one is not
    // known. Its files are removed and these entries lose kThumbPresent and
    // kFileDownloaded, so a listed one is fetched again.
    std::vector<std::int64_t> ambiguous_media_ids;
    std::vector<std::pair<std::int64_t, std::int64_t>> ambiguous;
};

LegacyFilePlan plan_legacy_files(const camera::artemis::MediaCatalog &catalog);

}  // namespace camera::store
