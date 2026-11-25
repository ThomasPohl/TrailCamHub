#include "media_files.hpp"

#include <algorithm>

namespace camera::store
{

namespace
{
// Reads a non-empty run of decimal digits starting at `pos`.
bool read_number(const std::string &text, std::size_t &pos, std::int64_t &out)
{
    const std::size_t start = pos;
    std::int64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        if (pos - start >= 12)
            return false;  // far beyond any dir/num
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    if (pos == start)
        return false;
    out = value;
    return true;
}
}  // namespace

std::string media_base_name(std::int64_t media_id)
{
    return std::to_string(media_id);
}

bool is_file_of_media(const std::string &name, std::int64_t media_id)
{
    const std::string base = media_base_name(media_id) + ".";
    return name.size() > base.size() && name.compare(0, base.size(), base) == 0;
}

bool parse_legacy_name(const std::string &name, std::int64_t &dir,
                       std::int64_t &num, std::string &rest)
{
    std::size_t pos = 0;
    if (!read_number(name, pos, dir) || pos >= name.size() || name[pos] != '_')
        return false;
    ++pos;
    if (!read_number(name, pos, num) || pos >= name.size() || name[pos] != '.')
        return false;
    ++pos;
    if (pos >= name.size())
        return false;
    rest = name.substr(pos);
    return true;
}

LegacyFilePlan plan_legacy_files(const camera::artemis::MediaCatalog &catalog)
{
    using Key = std::pair<std::int64_t, std::int64_t>;
    std::map<Key, std::vector<std::int64_t>> by_dir_num;
    for (const camera::artemis::CatalogEntry &e : catalog.entries())
        by_dir_num[{e.dir_num, e.media_num}].push_back(e.media_id);

    LegacyFilePlan plan;
    for (const auto &[key, ids] : by_dir_num) {
        if (ids.size() == 1) {
            plan.owner[key] = ids.front();
            continue;
        }
        plan.ambiguous.push_back(key);
        for (const std::int64_t id : ids)
            plan.ambiguous_media_ids.push_back(id);
    }
    std::sort(plan.ambiguous_media_ids.begin(), plan.ambiguous_media_ids.end());
    return plan;
}

}  // namespace camera::store
