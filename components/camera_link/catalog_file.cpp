#include "catalog_file.hpp"

#include <stdio.h>
#include <sys/stat.h>

namespace camera::store
{

namespace
{
bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}
}  // namespace

std::string replacement_path(const std::string &path)
{
    return path + ".tmp";
}

bool replace_file(const std::string &path)
{
    // FAT rename quirk: the target must not exist.
    remove(path.c_str());
    return rename(replacement_path(path).c_str(), path.c_str()) == 0;
}

bool recover_replaced_file(const std::string &path)
{
    const std::string tmp = replacement_path(path);
    if (exists(path) || !exists(tmp))
        return false;
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool SaveThrottle::note_change(std::int64_t now_us)
{
    if (pending_ == 0)
        oldest_us_ = now_us;
    ++pending_;
    return pending_ >= max_changes_ || now_us - oldest_us_ >= max_age_us_;
}

}  // namespace camera::store
