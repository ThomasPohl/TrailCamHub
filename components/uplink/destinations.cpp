/*
 * The destination besides the own backend (docs/modes.md):
 *
 *   WebDAV:   the hub copies new thumbnails, photos and videos into a
 *             folder of a NAS or cloud (Nextcloud, Synology, ...), one
 *             subfolder per camera.
 *
 * It keeps track of what was sent in the catalog: kReported for a
 * thumbnail, kVideoUploaded for a photo or video file.
 */

#include "destinations.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_actions.hpp"
#include "camera_wifi.h"
#include "device_info.hpp"
#include "http_util.hpp"
#include "hub_battery.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "mp4_body.hpp"
#include "mp4_mux.hpp"
#include "push_protocol.hpp"
#include "schedule_state.hpp"
#include "sdkconfig.h"
#include "sync_cameras.hpp"

namespace uplink
{

namespace
{

const char *TAG = "trailcamhub_push";

using camera::artemis::CatalogEntry;
using camera::artemis::MediaCatalog;

bool has(const CatalogEntry &entry, MediaCatalog::Flag flag)
{
    return (entry.flags & flag) != 0;
}

bool is_video(const CatalogEntry &entry)
{
    return entry.file_type == 2;
}

// Opens the MP4 of a recording; nullptr (and `reason`) if it cannot be built.
std::shared_ptr<Mp4Body> open_mp4(const std::string &camera_id,
                                  std::int64_t media_id, std::string &reason)
{
    auto body = std::make_shared<Mp4Body>(
        camera::store::media_path(camera_id, media_id, "h264"),
        camera::store::media_path(camera_id, media_id, "aac"));
    const Mp4Status status = body->open();
    if (status != Mp4Status::ok) {
        reason = body->failure(status);
        return nullptr;
    }
    return body;
}

BodyReader mp4_reader(const std::shared_ptr<Mp4Body> &body)
{
    return [body](std::uint64_t offset, std::uint8_t *buffer,
                  std::size_t size) -> std::int64_t {
        return body->file.read(offset, buffer, size);
    };
}

bool success(const HttpResult &result)
{
    return result.error == ESP_OK && result.status >= 200 &&
           result.status < 300;
}

// --- WebDAV ---

struct WebDav {
    const hub_config::Settings &settings;

    Request request(const std::vector<std::string> &segments,
                    esp_http_client_method_t method) const
    {
        Request r;
        r.url = push::join_url(settings.webdav_url, segments);
        r.method = method;
        r.user = settings.webdav_user;
        r.password = settings.webdav_password;
        return r;
    }

    // Creates a folder; one that exists already (405) is fine.
    bool make_folder(const std::vector<std::string> &segments,
                     std::string &error) const
    {
        const HttpResult result = send(request(segments, HTTP_METHOD_MKCOL));
        if (success(result) || result.status == 405) {
            return true;
        }
        error = "WebDAV folder " + push::join_url("", segments) + ": " +
                describe(result);
        return false;
    }

    bool put(const std::vector<std::string> &segments, const char *type,
             BodyReader body, std::uint64_t size, std::string &error,
             std::int64_t *read_us = nullptr) const
    {
        Request r = request(segments, HTTP_METHOD_PUT);
        r.headers.push_back({"Content-Type", type});
        r.body = std::move(body);
        r.body_size = size;
        const HttpResult result = send(r, read_us);
        if (success(result)) {
            return true;
        }
        error = "WebDAV upload " + push::join_url("", segments) + ": " +
                describe(result);
        return false;
    }

    bool put_file(const std::vector<std::string> &segments, const char *type,
                  const std::string &path, std::string &error) const
    {
        BodyReader body;
        std::uint64_t size = 0;
        if (file_body(path, body, size) != ESP_OK) {
            error = "missing on the SD card: " + path;
            return false;
        }
        return put(segments, type, body, size, error);
    }
};

}  // namespace

esp_err_t push_webdav(const hub_config::Settings &settings, bool over_modem,
                      Report &report)
{
    const WebDav dav{settings};
    std::vector<SyncCamera> cameras = load_cameras();
    std::string error;
    // A folder for the hub's files may not exist yet.
    if (!dav.make_folder({}, error)) {
        report.error = error;
        ESP_LOGE(TAG, "%s", error.c_str());
        return ESP_FAIL;
    }
    for (SyncCamera &camera : cameras) {
        report.cameras_reported++;
        bool folders = false;
        auto ensure_folders = [&]() {
            if (!folders) {
                folders = dav.make_folder({camera.id}, error) &&
                          dav.make_folder({camera.id, "thumb"}, error) &&
                          dav.make_folder({camera.id, "media"}, error);
            }
            return folders;
        };
        for (const CatalogEntry &entry : camera.catalog.entries()) {
            const char *ext = is_video(entry) ? "mp4" : "jpg";
            if (has(entry, MediaCatalog::kThumbPresent) &&
                !has(entry, MediaCatalog::kReported)) {
                if (!ensure_folders() ||
                    !dav.put_file({camera.id, "thumb",
                                   push::file_name(entry.media_time,
                                                   entry.media_id, "jpg")},
                                  "image/jpeg",
                                  camera::store::thumbnail_path(camera.id,
                                                                entry.media_id),
                                  error)) {
                    break;
                }
                camera.changed |= camera.catalog.set_flag(
                    entry.media_id, MediaCatalog::kReported);
                report.thumbs_uploaded++;
            }
            if (!has(entry, MediaCatalog::kFileDownloaded) ||
                has(entry, MediaCatalog::kVideoUploaded)) {
                continue;
            }
            const std::string name =
                push::file_name(entry.media_time, entry.media_id, ext);
            if (!is_video(entry)) {
                if (!ensure_folders() ||
                    !dav.put_file({camera.id, "media", name}, "image/jpeg",
                                  camera::store::media_path(
                                      camera.id, entry.media_id, "jpg"),
                                  error)) {
                    break;
                }
            } else {
                std::string reason;
                auto mp4 = open_mp4(camera.id, entry.media_id, reason);
                if (mp4 == nullptr) {
                    ESP_LOGW(TAG, "Video %lld of %s: %s",
                             (long long)entry.media_id, camera.id.c_str(),
                             reason.c_str());
                    continue;
                }
                if (mp4->file.size() > max_video_bytes(over_modem)) {
                    ESP_LOGW(TAG, "Video %lld of %s: %llu KB, too large here",
                             (long long)entry.media_id, camera.id.c_str(),
                             (unsigned long long)(mp4->file.size() >> 10));
                    continue;
                }
                const std::int64_t started_us = esp_timer_get_time();
                std::int64_t read_us = 0;
                if (!over_modem) {
                    camera_wifi_set_full_power(true);
                }
                const bool sent =
                    ensure_folders() &&
                    dav.put({camera.id, "media", name}, "video/mp4",
                            mp4_reader(mp4), mp4->file.size(), error, &read_us);
                if (!over_modem) {
                    camera_wifi_set_full_power(false);
                }
                if (!sent) {
                    break;
                }
                const std::int64_t ms =
                    (esp_timer_get_time() - started_us) / 1000;
                ESP_LOGI(TAG,
                         "Copied video %lld of %s to WebDAV: %llu KB in %lld "
                         "ms (reading the SD card %lld ms)",
                         (long long)entry.media_id, camera.id.c_str(),
                         (unsigned long long)(mp4->file.size() >> 10),
                         (long long)ms, (long long)(read_us / 1000));
                report.videos_uploaded++;
            }
            camera.changed |= camera.catalog.set_flag(
                entry.media_id, MediaCatalog::kVideoUploaded);
        }
        if (error.empty() && ensure_folders()) {
            // The camera's state (battery, SD card) next to its pictures.
            dav.put_file({camera.id, "camera.json"}, "application/json",
                         camera::store::camera_info_path(camera.id), error);
        }
        if (!error.empty()) {
            break;
        }
    }
    save_changed(cameras);
    if (!error.empty()) {
        report.error = error;
        ESP_LOGE(TAG, "%s", error.c_str());
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WebDAV: %d cameras, %d thumbnails and %d videos copied",
             report.cameras_reported, report.thumbs_uploaded,
             report.videos_uploaded);
    return ESP_OK;
}

}  // namespace uplink
