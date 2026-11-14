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
#include "esp_timer.h"

#include "camera_wifi.h"
#include "destinations.hpp"
#include "device_info.hpp"
#include "cycle_trace.hpp"
#include "hub_battery.hpp"
#include "http_util.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "modem_uplink.hpp"
#include "mp4_file_source.hpp"
#include "mp4_mux.hpp"
#include "ota_policy.hpp"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "sdkconfig.h"
#include "system_diagnostics.h"
#include "time_sync.h"
#include "uplink_protocol.hpp"
#include "video_request.hpp"

namespace uplink
{

namespace
{

const char *TAG = "trailcamhub_uplink";
constexpr size_t kUploadChunk = 4096;
// Keep a check-in well below the backend's limits (16 cameras, 2000 media);
// media not sent now go with the next sync.
constexpr std::size_t kMaxCamerasPerCheckin = 16;
#if CONFIG_SPIRAM
constexpr std::size_t kMaxMediaPerCamera = 300;
constexpr std::size_t kMaxMediaPerCheckin = 1000;
#else
// Without PSRAM the check-in has to fit next to the TLS session in about
// 80 KB of heap. A recording costs about 140 B there (its entry in the
// request and in the JSON); 300 of them left 7 KB free on the WROOM-32D,
// not enough for the response (#244). The backend asks for fewer thumbnails
// per check-in than that anyway, so a backlog does not reach the phone later
// because of this limit.
constexpr std::size_t kMaxMediaPerCamera = 40;
constexpr std::size_t kMaxMediaPerCheckin = 40;
#endif
#if CONFIG_SPIRAM
// Thumbnail uploads per check-in: left to the backend (30).
constexpr std::int64_t kMaxThumbUploads = -1;
#else
// The backend answers with a presigned URL of about 650 B per thumbnail. 30
// of them are 20 KB, which arrive as one TLS record of 16 KB, and mbedTLS
// needs that record in one block: "alloc(16749 bytes) failed" on the
// WROOM-32D (#244). Four keep the response at about 3 KB; the rest of a
// backlog follows with the next check-ins, newest first.
constexpr std::int64_t kMaxThumbUploads = 4;
#endif
constexpr std::size_t kVideoChunk = 16 * 1024;

#if CONFIG_CAMERA_DOWNLOAD_VIDEOS
constexpr bool kDownloadsVideos = true;
constexpr std::uint64_t kMaxVideoBytes =
    (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB * 1024 * 1024;
#else
constexpr bool kDownloadsVideos = false;
constexpr std::uint64_t kMaxVideoBytes = 50ull * 1024 * 1024;
#endif
#if CONFIG_CAMERA_DOWNLOAD_VIDEOS && CONFIG_MODEM_UPLINK
constexpr std::uint64_t kMaxVideoBytesModem =
    (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB_MODEM * 1024 * 1024;
#else
constexpr std::uint64_t kMaxVideoBytesModem = kMaxVideoBytes;
#endif

// POSTs a JSON body to the API with the device key.
// `body` is taken over and freed once it is sent, so that it does not sit
// next to the response.
HttpResult post_json(const std::string &url, const std::string &device_key,
                     std::string body)
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
            std::string().swap(body);
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

// PUTs a recording as MP4 to a presigned URL. The MP4 is built while it is
// sent, from the stored H.264 and AAC streams, so no copy is written.
// `too_large` is set to the reason (and nothing sent) if it exceeds the limit
// of the transport.
HttpResult put_mp4(const std::string &url, const std::string &camera_id,
                   std::int64_t media_id, bool over_modem, std::uint64_t &size,
                   std::string &too_large, std::int64_t &sd_read_us)
{
    HttpResult result;
    size = 0;
    sd_read_us = 0;
    too_large.clear();
    mp4::FileSource video(
        camera::store::media_path(camera_id, media_id, "h264"));
    mp4::FileSource audio(
        camera::store::media_path(camera_id, media_id, "aac"));
    if (!video.exists()) {
        result.error = ESP_ERR_NOT_FOUND;
        return result;
    }
    mp4::Mp4File file;
    if (!file.open(video,
                   audio.exists() && audio.size() > 0 ? &audio : nullptr)) {
        ESP_LOGW(TAG, "Cannot build the MP4 of %s media %lld: %s",
                 camera_id.c_str(), (long long)media_id, file.error().c_str());
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }
    size = file.size();
    VideoLimits limits;
    limits.wifi_bytes = kMaxVideoBytes;
    limits.modem_bytes = kMaxVideoBytesModem;
    too_large = video_size_failure(size, limits, over_modem);
    if (!too_large.empty()) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_PUT);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    // Must match the content type the URL was signed with.
    esp_http_client_set_header(client, "Content-Type", "video/mp4");
    // Modem sleep on the home Wi-Fi limits the upload to ~80 KB/s (#154).
    if (!over_modem) {
        camera_wifi_set_full_power(true);
    }
    result.error = esp_http_client_open(client, (int)size);
    if (result.error == ESP_OK) {
        std::vector<std::uint8_t> chunk(kVideoChunk);
        std::uint64_t position = 0;
        bool ok = true;
        while (ok && position < size) {
            const std::int64_t read_started_us = esp_timer_get_time();
            const std::int64_t n =
                file.read(position, chunk.data(), chunk.size());
            sd_read_us += esp_timer_get_time() - read_started_us;
            ok = n > 0 &&
                 esp_http_client_write(
                     client, reinterpret_cast<const char *>(chunk.data()),
                     (int)n) == (int)n;
            position += n > 0 ? (std::uint64_t)n : 0;
        }
        result = ok ? read_response(client) : HttpResult{};
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!over_modem) {
        camera_wifi_set_full_power(false);
    }
    return result;
}

void set_clock(std::int64_t unix_seconds)
{
    if (unix_seconds > 0) {
        time_sync_set(unix_seconds, TIME_SYNC_SOURCE_BACKEND);
    }
}

// One camera's state on the SD card during a sync.
struct CameraState {
    std::string id;
    camera::artemis::MediaCatalog catalog;
    std::vector<std::int64_t> sent;  // mediaIds of the media in this check-in
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
        report.sd_total_mb = info.sd_total_mb;
        report.sd_card = camera::artemis::sd_card_name(info.sd_card);
        report.sd_error = info.sd_error;
        report.temperature_c = info.temperature_c;
        report.charging = info.charging;
        report.solar_charging = info.solar_charging;
        report.bat_type = info.bat_type;
        report.ble_version = info.ble_version;
        report.camera_name = info.camera_name;
        const std::size_t limit = media_budget < kMaxMediaPerCamera
                                      ? media_budget
                                      : kMaxMediaPerCamera;
        for (const auto &e : state.catalog.without_flag(
                 camera::artemis::MediaCatalog::kReported, limit)) {
            const MediaRef ref = {e.media_id, e.dir_num, e.media_num,
                                  e.file_type, e.media_time};
            report.media.push_back(ref);
            state.sent.push_back(e.media_id);
        }
        media_budget -= report.media.size();
        const std::size_t waiting =
            state.catalog.size() -
            state.catalog.count_with_flag(
                camera::artemis::MediaCatalog::kReported) -
            report.media.size();
        if (waiting > 0) {
            ESP_LOGI(TAG,
                     "%s: %u recordings in this check-in, %u more with the "
                     "next ones",
                     id.c_str(), (unsigned)report.media.size(),
                     (unsigned)waiting);
        }
        request.cameras.push_back(std::move(report));
        cameras.push_back(std::move(state));
    }
}

// The schedule as it will be once this sync has succeeded.
ScheduleReport schedule_report()
{
    const schedule::Effective effective = schedule::effective();
    const schedule::Times times = schedule::next_times_after_sync();
    ScheduleReport report;
    report.present = true;
    report.camera_plan = effective.camera_plan;
    report.camera_plan_origin =
        schedule::origin_name(effective.camera_plan_origin);
    report.checkin_interval_s = effective.settings.uplink
                                    ? (std::int64_t)effective.checkin_min * 60
                                    : 0;
    report.checkin_origin = schedule::origin_name(effective.checkin_origin);
    report.next_camera_read_at = schedule::to_unix(times.camera);
    report.next_checkin_at = schedule::to_unix(times.checkin);
    report.sleeps = effective.sleeps;
    report.stretch = schedule::stretch_name(
        schedule::stretch(effective.settings, schedule::state()));
    return report;
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
    for (const std::int64_t media_id : camera.sent) {
        camera.catalog_changed |=
            camera.catalog.set_flag(media_id, MediaCatalog::kReported);
    }
}

// Looks for a newer release on GitHub and installs it if the battery and the
// uplink allow it; the scheduler restarts the hub after the cycle.
void update_firmware(const hub_config::Settings &settings)
{
    // The battery as read between cycles: at rest, not under the modem's
    // load.
    const schedule::State state = schedule::state();
    ota::Conditions conditions;
    conditions.battery_percent = state.hub_battery;
    conditions.charging = state.hub_charging;
    conditions.power_level = (int)schedule::hub_level(state);
    conditions.over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
    conditions.signal_dbm =
        conditions.over_modem ? modem_uplink::signal_dbm() : 0;
    // A firmware image is ~1.7 MB: no modem sleep while it may download.
    if (!conditions.over_modem) {
        camera_wifi_set_full_power(true);
    }
    ota::check_for_update(conditions);
    if (!conditions.over_modem) {
        camera_wifi_set_full_power(false);
    }
}

// Answers a requestVideo command: uploads the video if it is on the SD card,
// asks for a camera read if the request is new. Returns whether the command
// is settled (`out` is the answer); otherwise it is not acknowledged and
// comes again with the next check-in, with a fresh upload URL.
bool handle_video_command(CameraState *camera, const std::string &cam_id,
                          const Command &command, bool over_modem,
                          CommandResult &out, Report &report)
{
    using camera::artemis::MediaCatalog;
    const VideoDecision decision = decide_video(
        camera != nullptr ? &camera->catalog : nullptr, command.media_id,
        kDownloadsVideos, !command.upload_url.empty());
    switch (decision.step) {
    case VideoStep::request:
        camera->catalog_changed |= camera->catalog.set_flag(
            command.media_id, MediaCatalog::kVideoRequested);
        ESP_LOGI(TAG, "Video %lld of %s requested", (long long)command.media_id,
                 cam_id.c_str());
        schedule::request_camera_read("a video was requested");
        return false;
    case VideoStep::wait:
        return false;
    case VideoStep::done:
        out.done = true;
        return true;
    case VideoStep::upload: {
        std::uint64_t size = 0;
        std::string too_large;
        std::int64_t sd_read_us = 0;
        const std::int64_t started_us = esp_timer_get_time();
        const HttpResult put =
            put_mp4(command.upload_url, cam_id, command.media_id, over_modem,
                    size, too_large, sd_read_us);
        if (!too_large.empty()) {
            out.error = too_large;
            camera->catalog_changed |=
                camera->catalog.forget_video_request(command.media_id);
            ESP_LOGW(TAG, "Video %lld of %s: %s", (long long)command.media_id,
                     cam_id.c_str(), out.error.c_str());
            return true;
        }
        if (put.error == ESP_OK && put.status >= 200 && put.status < 300) {
            const std::int64_t ms = (esp_timer_get_time() - started_us) / 1000;
            // The SD share tells a slow card from a slow network (#154).
            ESP_LOGI(
                TAG,
                "Uploaded video %lld of %s over %s: %llu KB in %lld ms "
                "(%llu KB/s; reading the SD card %lld ms)",
                (long long)command.media_id, cam_id.c_str(),
                over_modem ? "the modem" : "Wi-Fi",
                (unsigned long long)(size >> 10), (long long)ms,
                (unsigned long long)(ms > 0 ? (size >> 10) * 1000 / ms : 0),
                (long long)(sd_read_us / 1000));
            camera->catalog_changed |= camera->catalog.set_flag(
                command.media_id, MediaCatalog::kVideoUploaded);
            report.videos_uploaded++;
            out.done = true;
            return true;
        }
        if (put.error == ESP_ERR_NOT_FOUND ||
            put.error == ESP_ERR_INVALID_STATE) {
            // The stored streams are gone or broken: fetch the video again.
            camera->catalog_changed |= camera->catalog.clear_flag(
                command.media_id, MediaCatalog::kFileDownloaded);
            camera->catalog_changed |= camera->catalog.set_flag(
                command.media_id, MediaCatalog::kVideoRequested);
        }
        // Tried again with the next check-in.
        ESP_LOGW(TAG, "Upload of video %lld of %s failed: %s",
                 (long long)command.media_id, cam_id.c_str(),
                 describe(put).c_str());
        return false;
    }
    case VideoStep::fail:
        out.error = video_failure_text(decision.failure);
        if (camera != nullptr) {
            camera->catalog_changed |=
                camera->catalog.forget_video_request(command.media_id);
        }
        ESP_LOGW(TAG, "Video %lld of %s: %s", (long long)command.media_id,
                 cam_id.c_str(), out.error.c_str());
        return true;
    }
    return false;
}

}  // namespace

namespace
{

// The memory headroom of the running cycle so far (the sync is its last
// part, and the one that needs the most).
MemoryReport memory_report()
{
    const cycle_trace::Memory memory = cycle_trace::memory();
    MemoryReport report;
    report.present = true;
    report.cycle = memory.cycle;
    report.heap_min_internal = memory.heap_min_internal;
    report.heap_min_psram = memory.heap_min_psram;
    report.stack_session = memory.stack_session;
    report.stack_uplink = memory.stack_uplink;
    report.stack_controller = memory.stack_controller;
    return report;
}

// The sync with the own backend (check-in, thumbnails, commands, ack).
esp_err_t run_backend(const hub_config::Settings &settings, Report &report)
{
    report = Report{};
    const std::string base = hub_config::api_base(settings.api_url);

    CheckinRequest request;
    request.hub_firmware = esp_app_get_description()->version;
    request.hub_board = ota::board();
    request.hub_variant = ota::variant();
    // -1 (left out of the check-in) on boards without a measurement.
    request.hub_bat_percent = hub_battery::percent();
    request.max_thumb_uploads = kMaxThumbUploads;
    request.schedule = schedule_report();
    request.memory = memory_report();
    std::vector<CameraState> cameras;
    collect_cameras(request, cameras);
    report.cameras_reported = (int)request.cameras.size();
    for (const CameraReport &c : request.cameras) {
        report.media_reported += (int)c.media.size();
    }
    if (cameras.empty()) {
        ESP_LOGW(TAG, "No camera data on the SD card yet; hub telemetry only");
    }

    // 1. One check-in for all cameras. Only the JSON is needed from here;
    // the request and, after parsing, the response text are freed, because
    // on a board without PSRAM they compete with the TLS session for the
    // heap.
    std::string body = build_checkin_json(request);
    request = CheckinRequest{};
    ESP_LOGI(TAG, "Check-in: %d cameras, %d recordings, %u B",
             report.cameras_reported, report.media_reported,
             (unsigned)body.size());
    diagnostics_log_memory("check-in");
    HttpResult checkin = post_json(base + "/v1/device/checkin",
                                   settings.device_key, std::move(body));
    CheckinResponse response;
    if (checkin.error != ESP_OK || checkin.status != 200 ||
        !parse_checkin_response(checkin.body, response)) {
        report.error = "check-in failed: " + describe(checkin);
        ESP_LOGE(TAG, "%s", report.error.c_str());
        return checkin.error != ESP_OK ? checkin.error : ESP_FAIL;
    }
    std::string().swap(checkin.body);
    report.server_time = response.server_time;
    // The firmware can reach the backend: a new one is kept.
    ota::confirm_running();
    set_clock(response.server_time);
    schedule::Remote remote;
    remote.has_camera_plan = response.has_camera_plan;
    remote.camera_plan = response.camera_plan;
    remote.checkin_interval_s = response.checkin_interval_s;
    schedule::apply_remote(remote);

    AckRequest ack;
    // Videos have a lower size limit over the cellular modem.
    const bool over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
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
            const camera::artemis::CatalogEntry *entry =
                camera != nullptr ? camera->catalog.find(upload.media_id)
                                  : nullptr;
            if (entry == nullptr ||
                !camera->catalog.has_flag(
                    upload.media_id,
                    camera::artemis::MediaCatalog::kThumbPresent)) {
                report.thumbs_not_on_sd++;
                continue;
            }
            const std::string path =
                camera::store::thumbnail_path(result.cam_id, upload.media_id);
            const HttpResult put = put_file(upload.url, path);
            if (put.error == ESP_ERR_NOT_FOUND) {
                report.thumbs_not_on_sd++;
                continue;
            }
            if (put.error == ESP_OK && put.status >= 200 && put.status < 300) {
                report.thumbs_uploaded++;
                camera_ack.uploaded_thumbs.push_back(upload.media_id);
            } else {
                ESP_LOGW(TAG, "Upload of %s media %lld failed: %s",
                         result.cam_id.c_str(), (long long)upload.media_id,
                         describe(put).c_str());
            }
        }

        // 3. Commands. A camera read is acknowledged at the check-in after
        // the read (the backend sends the command again until then). A video
        // request is acknowledged once the video was uploaded or cannot be
        // fetched (see handle_video_command()).
        for (const Command &command : result.commands) {
            CommandResult command_result;
            command_result.id = command.id;
            if (command.type == "readCamera") {
                const schedule::ReadRequests::Reply reply =
                    schedule::on_read_command(command.id, result.cam_id);
                if (reply == schedule::ReadRequests::Reply::pending) {
                    continue;
                }
                command_result.done =
                    reply == schedule::ReadRequests::Reply::done;
                if (!command_result.done) {
                    command_result.error = "the camera could not be read";
                }
                ESP_LOGI(TAG, "Camera read %s for %s: %s", command.id.c_str(),
                         result.cam_id.c_str(),
                         command_result.done ? "done" : "failed");
                camera_ack.commands.push_back(command_result);
                continue;
            }
            if (command.type == "requestVideo") {
                if (handle_video_command(camera, result.cam_id, command,
                                         over_modem, command_result, report)) {
                    camera_ack.commands.push_back(command_result);
                }
                continue;
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
    ESP_LOGI(TAG,
             "Sync done: %d/%d thumbnails uploaded (%d not on SD), %d "
             "videos uploaded",
             report.thumbs_uploaded, report.thumbs_requested,
             report.thumbs_not_on_sd, report.videos_uploaded);

    // 5. A newer firmware, last, so this sync is complete before it.
    update_firmware(settings);
    return ESP_OK;
}

// Tells the health check service (setting health_url) how the sync went;
// it warns the user when these calls stop.
void ping_health_check(const hub_config::Settings &settings, bool ok)
{
    if (settings.health_url.empty()) {
        return;
    }
    Request request;
    request.url = settings.health_url + (ok ? "" : "/fail");
    const HttpResult result = send(request);
    if (result.error != ESP_OK || result.status >= 300) {
        ESP_LOGW(TAG, "Health check: %s", describe(result).c_str());
    }
}

esp_err_t run_destination(const hub_config::Settings &settings, Report &report)
{
    const hub_config::Destination destination =
        hub_config::destination(settings);
    if (destination == hub_config::Destination::backend) {
        return run_backend(settings, report);
    }
    report = Report{};
    const bool over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
    const esp_err_t error = destination == hub_config::Destination::webdav
                                ? push_webdav(settings, over_modem, report)
                                : push_telegram(settings, over_modem, report);
    if (error == ESP_OK) {
        // The firmware reached its destination: a new one is kept. Then a
        // newer firmware, last, as with the backend.
        ota::confirm_running();
        update_firmware(settings);
    }
    return error;
}

}  // namespace

esp_err_t run(const hub_config::Settings &settings, Report &report)
{
    const esp_err_t error = run_destination(settings, report);
    ping_health_check(settings, error == ESP_OK);
    return error;
}

}  // namespace uplink
