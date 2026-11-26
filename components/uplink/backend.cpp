/*
 * The sync with the own backend (see backend/README.md): check-in, thumbnail
 * uploads, commands and acknowledgement. The server certificate is checked
 * against the ESP-IDF certificate bundle.
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
#include "sdkconfig.h"

#include "backend_config.hpp"
#include "backend_internal.hpp"
#include "camera_actions.hpp"
#include "camera_wifi.h"
#include "delete_request.hpp"
#include "destinations.hpp"
#include "data_usage_store.hpp"
#include "device_info.hpp"
#include "cycle_trace.hpp"
#include "hub_battery.hpp"
#include "http_util.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "modem_uplink.hpp"
#include "mp4_body.hpp"
#include "mp4_mux.hpp"
#include "ota_policy.hpp"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "sync_cameras.hpp"
#include "system_diagnostics.h"
#include "time_sync.h"
#include "uplink_protocol.hpp"
#include "video_request.hpp"

namespace uplink::backend
{

namespace
{

const char *TAG = "trailcamhub_uplink";

}  // namespace

namespace
{

// Stores the `power` setting the gallery asked for (#197). It is the hub's own
// setting afterwards, the same as `config set power`, so it holds until
// someone changes it again; it applies from the next start, because the mode
// is resolved once per start. A value the board cannot use is refused with
// the reason (once per value and start); nothing else about the hub changes.
void apply_remote_power(const std::string &wanted)
{
    static std::string s_refused;
    if (wanted.empty() || wanted == s_refused) {
        return;
    }
    hub_config::Settings stored;
    if (hub_config::load(stored) != ESP_OK || stored.power == wanted) {
        return;
    }
    std::string reason;
    if (hub_config::set(hub_config::Key::power, wanted, &reason) != ESP_OK) {
        s_refused = wanted;
        ESP_LOGW(TAG, "Ignoring power %s from the backend: %s", wanted.c_str(),
                 reason.empty() ? "could not be stored" : reason.c_str());
        return;
    }
    ESP_LOGI(TAG,
             "Power %s from the backend stored; it applies after the next "
             "restart",
             wanted.c_str());
}

}  // namespace

// The sync with the own backend (check-in, thumbnails, commands, ack).
esp_err_t run_backend(const hub_config::Settings &settings, Report &report)
{
    report = Report{};
    const std::string base = hub_config::api_base(settings.api_url);
    usage::init();
    {
        usage::Transport transport = usage::Transport::wifi;
        switch (hub_config::uplink_transport(settings)) {
        case hub_config::Transport::wifi:
            break;
        case hub_config::Transport::modem:
            transport = usage::Transport::modem;
            break;
        case hub_config::Transport::ethernet:
            transport = usage::Transport::ethernet;
            break;
        }
        usage::set_transport(transport);
    }
    // The totals are written once at the end of the sync, not per request.
    struct PersistUsage {
        ~PersistUsage()
        {
            usage::persist();
        }
    } persist_usage;

    CheckinRequest request;
    request.hub_firmware = esp_app_get_description()->version;
    request.features.push_back(kFeatureDeleteTargets);
    request.hub_board = ota::board();
    request.hub_variant = ota::variant();
    // -1 (left out of the check-in) on boards without a measurement.
    {
        hub_battery::Reading battery;
        if (hub_battery::read(battery)) {
            request.hub_bat_percent = battery.percent;
            request.hub_bat_millivolts = battery.millivolts;
            request.hub_charging = battery.charging ? 1 : 0;
        }
    }
    request.hub_power_mode =
        hub_config::power_name(hub_config::mode(settings).power);
    request.max_thumb_uploads = kMaxThumbUploads;
    request.schedule = schedule_report();
    request.memory = memory_report();
    switch (hub_config::uplink_transport(settings)) {
    case hub_config::Transport::wifi:
        request.hub_uplink = "wifi";
        break;
    case hub_config::Transport::modem:
        request.hub_uplink = "modem";
        request.modem_signal_dbm = modem_uplink::signal_dbm();
        request.modem_operator = modem_uplink::operator_name();
        break;
    case hub_config::Transport::ethernet:
        request.hub_uplink = "ethernet";
        break;
    }
    // The data volume since the last acknowledged check-in (#415). Only the
    // part counted before this snapshot is taken off after the answer.
    const usage::Totals usage_reported = usage::snapshot();
    for (std::size_t i = 0; i < usage::kTransports; i++) {
        const usage::Bytes &b = usage_reported.by_transport[i];
        if (!b.empty()) {
            request.data_usage.push_back(
                {usage::transport_name((usage::Transport)i),
                 usage::with_overhead(b.tx,
                                      CONFIG_UPLINK_DATA_OVERHEAD_PERCENT),
                 usage::with_overhead(b.rx,
                                      CONFIG_UPLINK_DATA_OVERHEAD_PERCENT)});
        }
    }
    // The last firmware update, once (#149).
    ota::UpdateRecord update;
    const bool has_update = ota::update_report(update);
    if (has_update) {
        request.firmware_update.present = true;
        request.firmware_update.from = update.from;
        request.firmware_update.to = update.to;
        request.firmware_update.result = ota::update_result_name(update.result);
        request.firmware_update.reason = update.reason;
        request.firmware_update.at = update.at;
    }
    std::vector<SyncCamera> cameras;
    SentMedia sent;
    collect_cameras(request, cameras, sent);
    // Every camera heard in the scans, read or not (#296).
    request.report_discovered = true;
    for (const camera::found::Entry &e : camera::store::load_found_cameras()) {
        request.discovered.push_back({e.address, e.name, e.rssi, e.seen_at});
    }
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
    usage::acknowledge(usage_reported);
    report.server_time = response.server_time;
    if (has_update) {
        ota::mark_update_reported(update);
    }
    // The firmware can reach the backend: a new one is kept.
    ota::confirm_running();
    set_clock(response.server_time);
    schedule::Remote remote;
    remote.has_camera_plan = response.has_camera_plan;
    remote.camera_plan = response.camera_plan;
    remote.checkin_interval_s = response.checkin_interval_s;
    remote.has_quiet = response.has_quiet;
    remote.quiet = response.quiet;
    remote.has_location = response.has_location;
    remote.location = response.location;
    schedule::apply_remote(remote);
    apply_remote_power(response.power_setting);
    // The cameras approved in the gallery: the next scans read only these
    // (#299). An empty list keeps reading every camera.
    if (response.has_approved_cameras) {
        camera::store::save_approved_cameras(response.approved_cameras);
    }

    AckRequest ack;
    // Videos have a lower size limit over the cellular modem.
    const bool over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
    // A deletion was queued or still waits: the camera session runs it.
    bool delete_read = false;
    bool settings_read = false;
    for (const CameraCheckin &result : response.cameras) {
        SyncCamera *camera = find_camera(cameras, result.cam_id);
        if (camera != nullptr) {
            apply_checkin_result(*camera, sent[camera - cameras.data()],
                                 result);
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
            }
            if (command.type == "deleteMedia") {
                if (handle_delete_command(result.cam_id, command,
                                          command_result, delete_read)) {
                    camera_ack.commands.push_back(command_result);
                }
                continue;
            }
            if (command.type == "setSettings") {
                if (handle_settings_command(result.cam_id, command,
                                            command_result, settings_read)) {
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

    save_changed(cameras);
    if (delete_read) {
        schedule::request_camera_read("a deletion was requested");
    }
    if (settings_read) {
        schedule::request_camera_read("a settings change was requested");
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

}  // namespace uplink::backend
