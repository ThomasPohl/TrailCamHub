/*
 * Talks HTTPS to the backend's device API (see backend/README.md). The
 * server certificate is checked against the ESP-IDF certificate bundle.
 */

#include "uplink.hpp"

#include <stdio.h>
#include <sys/time.h>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "device_info.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "time_sync.h"
#include "uplink_protocol.hpp"

namespace uplink
{

namespace
{

const char *TAG = "trailcamhub_uplink";
constexpr int kTimeoutMs = 20000;
constexpr int kMaxResponseBytes = 64 * 1024;
constexpr size_t kUploadChunk = 4096;
// Keep a check-in well below the backend's limits (16 cameras, 2000 media);
// media not sent now go with the next sync.
constexpr std::size_t kMaxCamerasPerCheckin = 16;
constexpr std::size_t kMaxMediaPerCamera = 300;
constexpr std::size_t kMaxMediaPerCheckin = 1000;

struct HttpResult {
    esp_err_t error = ESP_FAIL;
    int status = 0;
    std::string body;
};

esp_http_client_handle_t open_client(const std::string &url,
                                     esp_http_client_method_t method)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    // Presigned S3 URLs are long; the request line must fit.
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = false;
    return esp_http_client_init(&config);
}

HttpResult read_response(esp_http_client_handle_t client)
{
    HttpResult result;
    if (esp_http_client_fetch_headers(client) < 0) {
        result.error = ESP_ERR_HTTP_FETCH_HEADER;
        return result;
    }
    result.status = esp_http_client_get_status_code(client);
    char buffer[512];
    int read;
    while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        if (result.body.size() + (size_t)read > (size_t)kMaxResponseBytes) {
            result.error = ESP_ERR_INVALID_SIZE;
            return result;
        }
        result.body.append(buffer, read);
    }
    result.error = read < 0 ? ESP_FAIL : ESP_OK;
    return result;
}

// POSTs a JSON body to the API with the device key.
HttpResult post_json(const std::string &url, const std::string &device_key,
                     const std::string &body)
{
    HttpResult result;
    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_POST);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    const std::string authorization = "Bearer " + device_key;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    result.error = esp_http_client_open(client, body.size());
    if (result.error == ESP_OK) {
        if (esp_http_client_write(client, body.data(), body.size()) !=
            (int)body.size()) {
            result.error = ESP_FAIL;
        } else {
            result = read_response(client);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

// PUTs a file to a presigned URL (no API credentials involved).
HttpResult put_file(const std::string &url, const std::string &path)
{
    HttpResult result;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.error = ESP_ERR_NOT_FOUND;
        return result;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_PUT);
    if (client == nullptr || size <= 0) {
        fclose(file);
        if (client != nullptr) {
            esp_http_client_cleanup(client);
        }
        result.error =
            client == nullptr ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_SIZE;
        return result;
    }
    // Must match the content type the URL was signed with.
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    result.error = esp_http_client_open(client, size);
    if (result.error == ESP_OK) {
        std::vector<char> chunk(kUploadChunk);
        size_t n;
        bool ok = true;
        while (ok && (n = fread(chunk.data(), 1, chunk.size(), file)) > 0) {
            ok = esp_http_client_write(client, chunk.data(), n) == (int)n;
        }
        result = ok ? read_response(client) : HttpResult{};
    }
    fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

std::string describe(const HttpResult &result)
{
    if (result.error != ESP_OK) {
        return esp_err_to_name(result.error);
    }
    return "HTTP " + std::to_string(result.status) + " " +
           result.body.substr(0, 120);
}

void set_clock(std::int64_t unix_seconds)
{
    if (unix_seconds > 0) {
        time_sync_set(unix_seconds, timepolicy::Source::backend);
    }
}

// One camera's state on the SD card during a sync.
struct CameraState {
    std::string id;
    camera::artemis::MediaCatalog catalog;
    std::vector<MediaRef> sent;  // media in this check-in
    bool catalog_changed = false;
};

CameraState *find_camera(std::vector<CameraState> &cameras,
                         const std::string &id)
{
    for (CameraState &camera : cameras) {
        if (camera.id == id) {
            return &camera;
        }
    }
    return nullptr;
}

// Collects every camera on the SD card with the media not reported yet.
void collect_cameras(CheckinRequest &request, std::vector<CameraState> &cameras)
{
    std::size_t media_budget = kMaxMediaPerCheckin;
    for (const std::string &id : camera::store::list_cameras()) {
        if (request.cameras.size() >= kMaxCamerasPerCheckin) {
            ESP_LOGW(TAG,
                     "More than %u cameras; the rest waits for the next "
                     "sync",
                     (unsigned)kMaxCamerasPerCheckin);
            break;
        }
        camera::artemis::DeviceInfo info;
        CameraReport report;
        if (!camera::store::load_camera_info(id, info, report.read_at)) {
            continue;
        }
        CameraState state;
        state.id = id;
        camera::store::load_catalog(id, state.catalog);

        // The directory name is the sanitized camera id used everywhere.
        report.cam_id = id;
        report.model = info.model_name;
        report.firmware = info.fw_version;
        report.bat_percent = info.bat_percent;
        report.sd_free_mb = info.sd_free_mb;
        const std::size_t limit = media_budget < kMaxMediaPerCamera
                                      ? media_budget
                                      : kMaxMediaPerCamera;
        for (const auto &e : state.catalog.without_flag(
                 camera::artemis::MediaCatalog::kReported, limit)) {
            const MediaRef ref = {e.dir_num, e.media_num, e.file_type,
                                  e.media_time};
            report.media.push_back(ref);
            state.sent.push_back(ref);
        }
        media_budget -= report.media.size();
        request.cameras.push_back(report);
        cameras.push_back(std::move(state));
    }
}

// Remembers what the backend now knows, so the next check-in sends a delta.
void apply_checkin_result(CameraState &camera, const CameraCheckin &result)
{
    using camera::artemis::MediaCatalog;
    if (!result.accepted) {
        ESP_LOGW(TAG, "Camera %s belongs to another hub; skipped",
                 camera.id.c_str());
        return;
    }
    if (result.resync) {
        // New to the backend: send everything again next time.
        if (camera.catalog.count_with_flag(MediaCatalog::kReported) > 0) {
            camera.catalog.clear_flag_everywhere(MediaCatalog::kReported);
            camera.catalog_changed = true;
        }
        return;
    }
    for (const MediaRef &m : camera.sent) {
        camera.catalog_changed |=
            camera.catalog.set_flag(m.dir, m.num, MediaCatalog::kReported);
    }
}

}  // namespace

esp_err_t run(const hub_config::Settings &settings, Report &report)
{
    report = Report{};
    const std::string base = hub_config::api_base(settings.api_url);

    CheckinRequest request;
    request.hub_firmware = esp_app_get_description()->version;
    std::vector<CameraState> cameras;
    collect_cameras(request, cameras);
    report.cameras_reported = (int)request.cameras.size();
    for (const CameraReport &c : request.cameras) {
        report.media_reported += (int)c.media.size();
    }
    if (cameras.empty()) {
        ESP_LOGW(TAG, "No camera data on the SD card yet; hub telemetry only");
    }

    // 1. One check-in for all cameras.
    const HttpResult checkin =
        post_json(base + "/v1/device/checkin", settings.device_key,
                  build_checkin_json(request));
    CheckinResponse response;
    if (checkin.error != ESP_OK || checkin.status != 200 ||
        !parse_checkin_response(checkin.body, response)) {
        report.error = "check-in failed: " + describe(checkin);
        ESP_LOGE(TAG, "%s", report.error.c_str());
        return checkin.error != ESP_OK ? checkin.error : ESP_FAIL;
    }
    report.server_time = response.server_time;
    set_clock(response.server_time);

    AckRequest ack;
    for (const CameraCheckin &result : response.cameras) {
        CameraState *camera = find_camera(cameras, result.cam_id);
        if (camera != nullptr) {
            apply_checkin_result(*camera, result);
        }
        if (!result.accepted) {
            report.cameras_refused++;
            continue;
        }
        report.thumbs_requested += (int)result.thumb_uploads.size();
        report.commands_received += (int)result.commands.size();

        CameraAck camera_ack;
        camera_ack.cam_id = result.cam_id;

        // 2. Upload the thumbnails the backend is missing. Cameras the hub
        // has no data for (camera == nullptr) have nothing on the SD card.
        for (const ThumbUpload &upload : result.thumb_uploads) {
            const std::string path = camera::store::thumbnail_path(
                result.cam_id, upload.dir, upload.num);
            const HttpResult put = put_file(upload.url, path);
            if (put.error == ESP_ERR_NOT_FOUND) {
                report.thumbs_not_on_sd++;
                continue;
            }
            if (put.error == ESP_OK && put.status >= 200 && put.status < 300) {
                report.thumbs_uploaded++;
                camera_ack.uploaded_thumbs.push_back(
                    {upload.dir, upload.num, 0, 0});
            } else {
                ESP_LOGW(TAG, "Upload of %s %lld_%lld failed: %s",
                         result.cam_id.c_str(), (long long)upload.dir,
                         (long long)upload.num, describe(put).c_str());
            }
        }

        // 3. Commands. Video downloads are not implemented yet: remember the
        // request in the catalog and tell the backend it failed, so the user
        // sees it instead of waiting forever.
        for (const Command &command : result.commands) {
            CommandResult command_result;
            command_result.id = command.id;
            if (command.type == "requestVideo") {
                if (camera != nullptr) {
                    camera->catalog_changed |= camera->catalog.set_flag(
                        command.dir, command.num,
                        camera::artemis::MediaCatalog::kVideoRequested);
                }
                command_result.error = "video download is not supported by "
                                       "this firmware yet";
            } else {
                command_result.error = "unsupported command " + command.type;
            }
            ESP_LOGW(TAG, "Command %s (%s) for %s: %s", command.id.c_str(),
                     command.type.c_str(), result.cam_id.c_str(),
                     command_result.error.c_str());
            camera_ack.commands.push_back(command_result);
        }
        ack.cameras.push_back(camera_ack);
    }

    for (const CameraState &camera : cameras) {
        if (camera.catalog_changed &&
            !camera::store::save_catalog(camera.id, camera.catalog)) {
            ESP_LOGW(TAG, "Could not save the catalog of %s",
                     camera.id.c_str());
        }
    }
    ESP_LOGI(TAG,
             "Checked in %d cameras (%d refused), %d media; backend wanted "
             "%d thumbnails, %d commands",
             report.cameras_reported, report.cameras_refused,
             report.media_reported, report.thumbs_requested,
             report.commands_received);

    // 4. One acknowledgement for all cameras.
    if (!ack_is_empty(ack)) {
        const HttpResult acked = post_json(
            base + "/v1/device/ack", settings.device_key, build_ack_json(ack));
        if (acked.error != ESP_OK || acked.status != 200) {
            report.error = "acknowledgement failed: " + describe(acked);
            ESP_LOGE(TAG, "%s", report.error.c_str());
            return acked.error != ESP_OK ? acked.error : ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Sync done: %d/%d thumbnails uploaded (%d not on SD)",
             report.thumbs_uploaded, report.thumbs_requested,
             report.thumbs_not_on_sd);
    return ESP_OK;
}

}  // namespace uplink
