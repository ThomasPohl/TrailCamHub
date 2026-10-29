#include "media_store.hpp"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>

#include "esp_log.h"
#include "sd_card.h"

namespace camera::store
{

namespace
{
const char *TAG = "trailcamhub_store";

std::string camera_dir(const std::string &camera_id)
{
    return std::string(SD_CARD_MOUNT_POINT) + "/" +
           sanitize_camera_id(camera_id);
}

// Creates a directory, tolerating "already exists".
bool ensure_dir(const std::string &path)
{
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST)
        return true;
    ESP_LOGW(TAG, "mkdir %s failed: errno %d", path.c_str(), errno);
    return false;
}

bool read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    char buf[512];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

bool write_file(const std::string &path, const void *data, size_t size)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "open %s for write failed: errno %d", path.c_str(),
                 errno);
        return false;
    }
    const bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0 || !ok) {
        ESP_LOGW(TAG, "write %s failed", path.c_str());
        return false;
    }
    return true;
}
}  // namespace

bool load_catalog(const std::string &camera_id,
                  camera::artemis::MediaCatalog &out)
{
    const std::string path = camera_dir(camera_id) + "/catalog.csv";
    std::string text;
    if (!read_file(path, text)) {
        // No file yet is a normal first run: start with an empty catalog.
        out.clear();
        return true;
    }
    if (!out.parse(text)) {
        ESP_LOGW(TAG, "catalog %s unrecognised; starting empty", path.c_str());
        out.clear();
        return false;
    }
    return true;
}

bool save_catalog(const std::string &camera_id,
                  const camera::artemis::MediaCatalog &catalog)
{
    const std::string dir = camera_dir(camera_id);
    if (!ensure_dir(dir))
        return false;
    const std::string path = dir + "/catalog.csv";
    const std::string tmp = path + ".tmp";
    const std::string text = catalog.serialize();
    if (!write_file(tmp, text.data(), text.size()))
        return false;
    // Replace atomically; remove any stale target first (FAT rename quirk).
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        ESP_LOGW(TAG, "rename %s failed: errno %d", path.c_str(), errno);
        return false;
    }
    return true;
}

bool write_thumbnail(const std::string &camera_id, std::int64_t dir,
                     std::int64_t num, const std::uint8_t *jpeg,
                     std::size_t size, std::int64_t media_time)
{
    if (jpeg == nullptr || size == 0)
        return false;
    const std::string base = camera_dir(camera_id);
    if (!ensure_dir(base))
        return false;
    const std::string thumbs = base + "/thumb";
    if (!ensure_dir(thumbs))
        return false;
    char name[48];
    snprintf(name, sizeof(name), "/%lld_%lld.jpg", (long long)dir,
             (long long)num);
    const std::string path = thumbs + name;
    if (!write_file(path, jpeg, size))
        return false;
    // Set the file's timestamps to the recording time so the thumbnail dates
    // match the capture rather than the sync. media_time is Unix seconds; <= 0
    // means the camera clock was unset, so leave the default (sync) time.
    if (media_time > 0) {
        struct utimbuf times;
        times.actime = static_cast<time_t>(media_time);
        times.modtime = static_cast<time_t>(media_time);
        if (utime(path.c_str(), &times) != 0) {
            ESP_LOGW(TAG, "utime %s failed: errno %d", path.c_str(), errno);
        }
    }
    return true;
}

}  // namespace camera::store
