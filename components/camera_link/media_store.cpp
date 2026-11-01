#include "media_store.hpp"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>

#include "artemis_json.hpp"
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
    const std::string path = thumbnail_path(camera_id, dir, num);
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

// Larger stdio buffer for downloads: fewer, larger FAT writes.
constexpr size_t kMediaWriteBuffer = 8 * 1024;

std::FILE *create_camera_file(const std::string &camera_id, const char *name)
{
    const std::string dir = camera_dir(camera_id);
    if (!ensure_dir(dir))
        return nullptr;
    const std::string path = dir + "/" + name;
    std::FILE *file = fopen(path.c_str(), "w");
    if (file == nullptr) {
        ESP_LOGW(TAG, "create %s failed: errno %d", path.c_str(), errno);
    }
    return file;
}

bool append_line(const std::string &camera_id, const char *name,
                 const char *header, const std::string &line)
{
    const std::string dir = camera_dir(camera_id);
    if (!ensure_dir(dir))
        return false;
    const std::string path = dir + "/" + name;
    struct stat st;
    const bool exists = stat(path.c_str(), &st) == 0;
    FILE *file = fopen(path.c_str(), "a");
    if (file == nullptr) {
        ESP_LOGW(TAG, "open %s for append failed: errno %d", path.c_str(),
                 errno);
        return false;
    }
    bool ok = true;
    if (!exists) {
        ok = fprintf(file, "%s\n", header) >= 0;
    }
    ok = ok && fprintf(file, "%s\n", line.c_str()) >= 0;
    ok = fclose(file) == 0 && ok;
    return ok;
}

bool MediaFileWriter::open(const std::string &camera_id, std::int64_t dir,
                           std::int64_t num, const char *extension,
                           bool buffered)
{
    abort();
    const std::string camera = camera_dir(camera_id);
    const std::string media = camera + "/media";
    if (!ensure_dir(camera) || !ensure_dir(media))
        return false;
    char name[48];
    snprintf(name, sizeof(name), "/%lld_%lld", (long long)dir, (long long)num);
    base_ = media + name;
    extension_ = extension != nullptr ? extension : "";
    path_.clear();
    head_size_ = 0;
    size_ = 0;
    const std::string part = part_path();
    file_ = fopen(part.c_str(), "wb");
    if (file_ == nullptr) {
        ESP_LOGW(TAG, "open %s for write failed: errno %d", part.c_str(),
                 errno);
        return false;
    }
    if (buffered) {
        setvbuf(file_, nullptr, _IOFBF, kMediaWriteBuffer);
    } else {
        setvbuf(file_, nullptr, _IONBF, 0);
    }
    return true;
}

bool MediaFileWriter::write(const std::uint8_t *data, std::size_t size)
{
    if (file_ == nullptr)
        return false;
    const size_t head = std::min(size, sizeof(head_) - head_size_);
    memcpy(head_ + head_size_, data, head);
    head_size_ += head;
    if (fwrite(data, 1, size, file_) != size) {
        ESP_LOGW(TAG, "write %s failed: errno %d", part_path().c_str(), errno);
        return false;
    }
    size_ += size;
    return true;
}

bool MediaFileWriter::finish(std::int64_t media_time)
{
    if (file_ == nullptr)
        return false;
    const std::string part = part_path();
    const bool closed = fclose(file_) == 0;
    file_ = nullptr;
    if (!closed) {
        ESP_LOGW(TAG, "close %s failed: errno %d", part.c_str(), errno);
        remove(part.c_str());
        return false;
    }
    const std::string path =
        base_ + "." +
        (extension_.empty() ? media_extension(head_, head_size_)
                            : extension_.c_str());
    remove(path.c_str());
    if (rename(part.c_str(), path.c_str()) != 0) {
        ESP_LOGW(TAG, "rename %s failed: errno %d", path.c_str(), errno);
        remove(part.c_str());
        return false;
    }
    path_ = path;
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

void MediaFileWriter::abort()
{
    if (file_ == nullptr)
        return;
    fclose(file_);
    file_ = nullptr;
    remove(part_path().c_str());
}

std::string MediaFileWriter::part_path() const
{
    // With a fixed extension several files of one item can be open at once.
    return extension_.empty() ? base_ + ".part"
                              : base_ + "." + extension_ + ".part";
}

std::string thumbnail_path(const std::string &camera_id, std::int64_t dir,
                           std::int64_t num)
{
    char name[48];
    snprintf(name, sizeof(name), "/thumb/%lld_%lld.jpg", (long long)dir,
             (long long)num);
    return camera_dir(camera_id) + name;
}

bool save_camera_info(const camera::artemis::DeviceInfo &info,
                      std::int64_t read_at)
{
    const std::string dir = camera_dir(info.dev_id);
    if (!ensure_dir(dir))
        return false;
    std::string text = camera::artemis::serialize_device_info(info);
    // Append the read time as one more field of the same flat object.
    // A clock before 2024 has not been set yet (no uplink since boot).
    constexpr std::int64_t kClockValidFrom = 1704067200;
    text.insert(text.size() - 1,
                ",\"readAt\":" +
                    std::to_string(read_at >= kClockValidFrom ? read_at : 0));
    // The file is rewritten at the start of every session: keep the time of
    // the last completed sync.
    std::string previous;
    std::int64_t last_sync = 0;
    if (read_file(dir + "/camera.json", previous) &&
        camera::artemis::json_find_int(previous, "lastSyncAt", last_sync)) {
        text = camera::artemis::json_set_int(text, "lastSyncAt", last_sync);
    }
    return write_file(dir + "/camera.json", text.data(), text.size());
}

bool load_camera_info(const std::string &camera_id,
                      camera::artemis::DeviceInfo &out, std::int64_t &read_at)
{
    std::string text;
    read_at = 0;
    if (!read_file(camera_dir(camera_id) + "/camera.json", text) ||
        !camera::artemis::parse_device_info(text, out)) {
        return false;
    }
    camera::artemis::json_find_int(text, "readAt", read_at);
    return true;
}

bool mark_camera_synced(const std::string &camera_id, std::int64_t at)
{
    const std::string path = camera_dir(camera_id) + "/camera.json";
    std::string text;
    if (!read_file(path, text)) {
        return false;
    }
    constexpr std::int64_t kClockValidFrom = 1704067200;
    text = camera::artemis::json_set_int(text, "lastSyncAt",
                                         at >= kClockValidFrom ? at : -1);
    return write_file(path, text.data(), text.size());
}

std::vector<std::string> list_cameras()
{
    std::vector<std::string> ids;
    DIR *root = opendir(SD_CARD_MOUNT_POINT);
    if (root == nullptr)
        return ids;
    struct dirent *entry;
    while ((entry = readdir(root)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        struct stat info;
        const std::string marker =
            std::string(SD_CARD_MOUNT_POINT) + "/" + name + "/camera.json";
        if (stat(marker.c_str(), &info) == 0)
            ids.push_back(name);
    }
    closedir(root);
    return ids;
}

}  // namespace camera::store
