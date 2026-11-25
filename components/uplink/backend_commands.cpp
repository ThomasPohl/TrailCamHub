/*
 * Commands in the backend's check-in response: deleteMedia, setSettings and
 * requestVideo.
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
#include "camera_settings.hpp"
#include "camera_wifi.h"
#include "delete_request.hpp"
#include "destinations.hpp"
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

// Answers a deleteMedia command (#260): queues the deletion for the camera's
// next session (once, recognised by the command id) and answers it when the
// queue has its result. Returns whether the command is settled (`out` is the
// answer); `read_needed` is set when a camera session should run it now.
bool handle_delete_command(const std::string &cam_id, const Command &command,
                           CommandResult &out, bool &read_needed)
{
    namespace act = camera::action;
    const std::uint8_t scope =
        delete_scope(command.has_targets, command.targets);
    if (scope == 0) {
        out.error = "the command names no target the hub knows";
        ESP_LOGW(TAG, "Deletion of %lld on %s: %s", (long long)command.media_id,
                 cam_id.c_str(), out.error.c_str());
        return true;
    }
    act::Action action{act::Type::delete_media, command.media_id, cam_id, "",
                       scope};
    const act::ActionQueue::AddResult added = act::service::submit(
        action, act::Source::backend, delete_key(command.id));
    act::Entry entry;
    const bool known =
        added.kind == act::ActionQueue::AddResult::Kind::duplicate &&
        act::service::find(added.id, entry);
    const DeleteDecision decision =
        decide_delete(added, known ? &entry : nullptr);
    read_needed |= decision.read_needed;
    if (added.kind == act::ActionQueue::AddResult::Kind::added) {
        ESP_LOGI(TAG, "Deletion of %lld on %s queued (action %u, %s)",
                 (long long)command.media_id, cam_id.c_str(),
                 (unsigned)added.id, act::scope_name(scope));
    }
    if (!decision.answer) {
        return false;
    }
    out.done = decision.done;
    out.error = decision.error;
    ESP_LOGI(TAG, "Deletion of %lld on %s: %s", (long long)command.media_id,
             cam_id.c_str(), decision.done ? "done" : decision.error.c_str());
    return true;
}

// Answers a setSettings command (#368) like a deletion: checks the change,
// queues it for the camera's next session (once, recognised by the command
// id) and answers it when the queue has its result. A change the hub cannot
// read is refused at once.
bool handle_settings_command(const std::string &cam_id, const Command &command,
                             CommandResult &out, bool &read_needed)
{
    namespace act = camera::action;
    std::vector<camera::settings::Value> change;
    std::string error;
    if (!camera::settings::parse_change(command.settings, change, error)) {
        out.done = false;
        out.error = error;
        ESP_LOGW(TAG, "Settings for %s refused: %s", cam_id.c_str(),
                 error.c_str());
        return true;
    }
    act::Action action{act::Type::settings, 0, cam_id,
                       camera::settings::encode_change(change)};
    const act::ActionQueue::AddResult added = act::service::submit(
        action, act::Source::backend, delete_key(command.id));
    act::Entry entry;
    const bool known =
        added.kind == act::ActionQueue::AddResult::Kind::duplicate &&
        act::service::find(added.id, entry);
    const DeleteDecision decision =
        decide_settings(added, known ? &entry : nullptr);
    read_needed |= decision.read_needed;
    if (added.kind == act::ActionQueue::AddResult::Kind::added) {
        ESP_LOGI(TAG, "Settings %s for %s queued (action %u)",
                 action.params.c_str(), cam_id.c_str(), (unsigned)added.id);
    }
    if (!decision.answer) {
        return false;
    }
    out.done = decision.done;
    out.error = decision.error;
    ESP_LOGI(TAG, "Settings for %s: %s", cam_id.c_str(),
             decision.done ? "done" : decision.error.c_str());
    return true;
}

// Answers a requestVideo command: uploads the video if it is on the SD card,
// asks for a camera read if the request is new. Returns whether the command
// is settled (`out` is the answer); otherwise it is not acknowledged and
// comes again with the next check-in, with a fresh upload URL.
bool handle_video_command(SyncCamera *camera, const std::string &cam_id,
                          const Command &command, bool over_modem,
                          CommandResult &out, Report &report)
{
    using camera::artemis::MediaCatalog;
    const VideoDecision decision = decide_video(
        camera != nullptr ? &camera->catalog : nullptr, command.media_id,
        kDownloadsVideos, !command.upload_url.empty());
    switch (decision.step) {
    case VideoStep::request: {
        // Clearly too long for the modem: refused without a camera read.
        VideoLimits limits;
        limits.wifi_bytes = kMaxVideoBytes;
        limits.modem_bytes = kMaxVideoBytesModem;
        const camera::artemis::CatalogEntry *entry =
            camera->catalog.find(command.media_id);
        const std::string too_long = video_estimate_failure(
            entry != nullptr ? entry->duration_ms : 0, limits, over_modem);
        if (!too_long.empty()) {
            out.error = too_long;
            ESP_LOGW(TAG, "Video %lld of %s: %s", (long long)command.media_id,
                     cam_id.c_str(), out.error.c_str());
            return true;
        }
        // Asked for now: a video the hub deleted its copy of is fetched again.
        camera->changed |= camera->catalog.clear_flag(
            command.media_id, MediaCatalog::kHubDeleted);
        camera->changed |= camera->catalog.set_flag(
            command.media_id, MediaCatalog::kVideoRequested);
        ESP_LOGI(TAG, "Video %lld of %s requested", (long long)command.media_id,
                 cam_id.c_str());
        schedule::request_camera_read("a video was requested");
        return false;
    }
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
            camera->changed |=
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
            camera->changed |= camera->catalog.set_flag(
                command.media_id, MediaCatalog::kVideoUploaded);
            report.videos_uploaded++;
            out.done = true;
            return true;
        }
        if (put.error == ESP_ERR_NOT_FOUND ||
            put.error == ESP_ERR_INVALID_STATE) {
            // The stored streams are gone or broken: fetch the video again.
            camera->changed |= camera->catalog.clear_flag(
                command.media_id, MediaCatalog::kFileDownloaded);
            camera->changed |= camera->catalog.set_flag(
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
            camera->changed |=
                camera->catalog.forget_video_request(command.media_id);
        }
        ESP_LOGW(TAG, "Video %lld of %s: %s", (long long)command.media_id,
                 cam_id.c_str(), out.error.c_str());
        return true;
    }
    return false;
}

}  // namespace uplink::backend
