/*
 * What a check-in with the backend carries and what the answer changes:
 * the cameras and their new media, the schedule, the memory report, the
 * clock, and what the backend now knows afterwards.
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

void set_clock(std::int64_t unix_seconds)
{
    if (unix_seconds > 0) {
        time_sync_set(unix_seconds, TIME_SYNC_SOURCE_BACKEND);
    }
}

// Collects every camera on the SD card with the media not reported yet.
void collect_cameras(CheckinRequest &request, std::vector<SyncCamera> &cameras,
                     SentMedia &sent)
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
        SyncCamera state = load_camera(id);
        SentIds sent_ids;

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
        report.ble_address = camera::store::camera_ble_address(id);
        const std::size_t limit = media_budget < kMaxMediaPerCamera
                                      ? media_budget
                                      : kMaxMediaPerCamera;
        // Entries deleted from the card are not reported as media (the
        // gallery would show them again), only as removed.
        const std::vector<camera::artemis::CatalogEntry> unreported =
            state.catalog.unreported_present(state.catalog.size());
        for (const auto &e : unreported) {
            if (report.media.size() >= limit) {
                break;
            }
            const MediaRef ref = {e.media_id, e.dir_num, e.media_num,
                                  e.file_type, e.media_time};
            report.media.push_back(ref);
            sent_ids.media.push_back(e.media_id);
        }
        media_budget -= report.media.size();
        const std::size_t waiting = unreported.size() - report.media.size();
        // Only ids, so they do not count against the media budget.
        report.removed = state.catalog.unreported_gone(kMaxMediaPerCamera);
        sent_ids.removed = report.removed;
        if (waiting > 0) {
            ESP_LOGI(TAG,
                     "%s: %u recordings in this check-in, %u more with the "
                     "next ones",
                     id.c_str(), (unsigned)report.media.size(),
                     (unsigned)waiting);
        }
        request.cameras.push_back(std::move(report));
        cameras.push_back(std::move(state));
        sent.push_back(std::move(sent_ids));
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
void apply_checkin_result(SyncCamera &camera, const SentIds &sent,
                          const CameraCheckin &result)
{
    using camera::artemis::MediaCatalog;
    if (!result.accepted) {
        ESP_LOGW(TAG, "Camera %s belongs to another hub; skipped",
                 camera.id.c_str());
        return;
    }
    if (result.resync) {
        // New to the backend: send everything again next time.
        if (camera.catalog.count_with_flag(MediaCatalog::kReported) > 0 ||
            camera.catalog.count_with_flag(MediaCatalog::kGoneReported) > 0) {
            camera.catalog.clear_flag_everywhere(MediaCatalog::kReported);
            camera.catalog.clear_flag_everywhere(MediaCatalog::kGoneReported);
            camera.changed = true;
        }
        return;
    }
    for (const std::int64_t media_id : sent.media) {
        camera.changed |=
            camera.catalog.set_flag(media_id, MediaCatalog::kReported);
    }
    for (const std::int64_t media_id : sent.removed) {
        camera.changed |=
            camera.catalog.set_flag(media_id, MediaCatalog::kGoneReported);
    }
}

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

}  // namespace uplink::backend
