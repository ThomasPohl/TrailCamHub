#pragma once

// Persists per-camera media on the SD card: the catalog CSV, the thumbnail
// JPEGs and the downloaded photos and videos, laid out as
//     /sdcard/<camera-id>/catalog.csv
//     /sdcard/<camera-id>/thumb/<dir>_<num>.jpg
//     /sdcard/<camera-id>/media/<dir>_<num>.<jpg|mp4|mov|avi|bin>
// Only the filesystem-id sanitiser and the file-type detection are pure (and
// host-tested); the read/write functions use stdio against the mounted SD card.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "device_info.hpp"
#include "media_catalog.hpp"

namespace camera::store
{

// Maps a camera identity to a safe single path segment: keeps [A-Za-z0-9._-],
// turns anything else into '_', caps the length, and never yields an empty
// name or "."/".." (which would escape or collide on the filesystem).
inline std::string sanitize_camera_id(const std::string &id)
{
    std::string out;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 48)
        out.resize(48);
    if (out.empty())
        out = "unknown";
    if (out == "." || out == "..")
        out = "_" + out;
    return out;
}

// File-name extension for a downloaded media file, chosen from its first
// bytes (at least 12 are needed): JPEG, MP4/MOV (ISO base media) or AVI.
// Anything else is stored as "bin".
inline const char *media_extension(const std::uint8_t *head, std::size_t size)
{
    if (size >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff)
        return "jpg";
    if (size >= 12 && std::memcmp(head + 4, "ftyp", 4) == 0)
        return std::memcmp(head + 8, "qt  ", 4) == 0 ? "mov" : "mp4";
    if (size >= 12 && std::memcmp(head, "RIFF", 4) == 0 &&
        std::memcmp(head + 8, "AVI ", 4) == 0)
        return "avi";
    return "bin";
}

// Loads the catalog for `camera_id`. A missing file is treated as an empty
// catalog and still returns true; false means the SD card or file could not be
// read, or the file was not a valid catalog.
bool load_catalog(const std::string &camera_id,
                  camera::artemis::MediaCatalog &out);

// Writes the catalog atomically (temp file + rename). Returns false on error.
bool save_catalog(const std::string &camera_id,
                  const camera::artemis::MediaCatalog &catalog);

// Writes a thumbnail JPEG for (dir, num), creating directories as needed, and
// sets the file's access/modification times to `media_time` (Unix seconds) so
// the thumbnail's date reflects the capture. A media_time <= 0 leaves the
// default timestamp.
bool write_thumbnail(const std::string &camera_id, std::int64_t dir,
                     std::int64_t num, const std::uint8_t *jpeg,
                     std::size_t size, std::int64_t media_time);

// Creates <camera-id>/<name> for writing (replacing an existing file) and
// returns it, or nullptr on error. For diagnostics.
std::FILE *create_camera_file(const std::string &camera_id, const char *name);

// Appends `line` (without newline) to <camera-id>/<name>; a new file starts
// with `header`. For statistics. Returns false on error.
bool append_line(const std::string &camera_id, const char *name,
                 const char *header, const std::string &line);

// Writes one downloaded media file. The bytes go to a ".part" file in
// <camera-id>/media/; finish() renames it to <dir>_<num>.<extension>, so an
// interrupted download never looks like a complete file. Without a given
// extension it is detected from the content.
class MediaFileWriter
{
  public:
    MediaFileWriter() = default;
    MediaFileWriter(const MediaFileWriter &) = delete;
    MediaFileWriter &operator=(const MediaFileWriter &) = delete;
    ~MediaFileWriter()
    {
        abort();
    }

    // Without `buffered` the file has no stdio buffer (8 KB of internal RAM
    // each); for writes of several KB from internal RAM, as the SD writer
    // makes them.
    bool open(const std::string &camera_id, std::int64_t dir, std::int64_t num,
              const char *extension = nullptr, bool buffered = true);
    bool write(const std::uint8_t *data, std::size_t size);
    // Closes the file, gives it its final name and dates it by `media_time`
    // (Unix seconds; <= 0 keeps the default). Returns false on error, in
    // which case the partial file is removed.
    bool finish(std::int64_t media_time);
    // Closes and removes the partial file, if one is open.
    void abort();

    bool is_open() const
    {
        return file_ != nullptr;
    }
    // Bytes written so far.
    std::uint64_t size() const
    {
        return size_;
    }
    // Final path, valid after a successful finish().
    const std::string &path() const
    {
        return path_;
    }

  private:
    std::string part_path() const;

    std::FILE *file_ = nullptr;
    std::string base_;       // path without extension
    std::string extension_;  // empty: detect from the content
    std::string path_;
    std::uint8_t head_[12] = {};
    std::size_t head_size_ = 0;
    std::uint64_t size_ = 0;
};

// Path of the stored thumbnail for (dir, num); the file may not exist.
std::string thumbnail_path(const std::string &camera_id, std::int64_t dir,
                           std::int64_t num);

// Stores the camera's device info next to its catalog, at
// /sdcard/<camera-id>/camera.json, together with the time it was read
// (Unix seconds, 0 = unknown). An upload sends every camera stored this way.
bool save_camera_info(const camera::artemis::DeviceInfo &info,
                      std::int64_t read_at);
bool load_camera_info(const std::string &camera_id,
                      camera::artemis::DeviceInfo &out, std::int64_t &read_at);

// Records in camera.json that a sync with the camera has completed:
// "lastSyncAt" is the time in Unix seconds, or -1 if the clock has not been set
// yet (the sync happened, its time is unknown). Without the field the camera
// has never been synced. False if there is no camera.json to update.
bool mark_camera_synced(const std::string &camera_id, std::int64_t at);

// Ids (sanitized directory names) of all cameras with a camera.json.
std::vector<std::string> list_cameras();

}  // namespace camera::store
