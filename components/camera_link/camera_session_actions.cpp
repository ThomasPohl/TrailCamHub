// The camera session's queued actions (#194) and the phases around them
// (camera_session.hpp).

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "artemis_crypto.hpp"
#include "artemis_json.hpp"
#include "camera_action.hpp"
#include "camera_actions.hpp"
#include "artemis_login.hpp"
#include "camera_clock.hpp"
#include "artemis_message.hpp"
#include "artemis_reassembler.hpp"
#include "camera_handshake.hpp"
#include "camera_link_io.hpp"
#include "time_sync.h"
#include "camera_session.hpp"
#include "camera_session_config.hpp"
#include "camera_settings.hpp"
#include "camera_udp_transport.h"
#include "catalog_file.hpp"
#include "deferred_command.hpp"
#include "bulk_reassembler.hpp"
#include "bulk_side_channel.hpp"
#include "bulk_stream.hpp"
#include "device_info.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "camera_udp_packets.hpp"
#include "mbedtls/base64.h"
#include "media_list.hpp"
#include "media_list_flow.hpp"
#include "media_transfer.hpp"
#include "pppp_session.hpp"
#include "sd_writer.hpp"
#include "thumbnail_queue.hpp"
#include "playback_download.hpp"
#include "sdkconfig.h"

namespace camera::link
{

void CameraSession::begin_listing_phase()
{
    if (settings_.actions_only &&
        !camera::action::service::pending_needs_list()) {
        ESP_LOGI(TAG, "The queued actions need no media list; done");
        work_done_ = true;
        return;
    }
    catalog_.begin_listing();
    scan_list_.reset();
    request_scan_page();
}

void CameraSession::after_list_actions()
{
    if (settings_.actions_only) {
        work_done_ = true;
        return;
    }
    begin_thumbnails();
}

bool CameraSession::delete_hub_copies(std::int64_t media_id)
{
    const camera::store::HubFilesRemoved removed =
        camera::store::delete_hub_files(camera_id_, media_id);
    catalog_.mark_hub_deleted(media_id);
    ESP_LOGI(TAG, "Deleted the hub's copies of %lld: %u file(s), %u failed",
             (long long)media_id, removed.removed, removed.failed);
    return removed.failed == 0;
}

// Sends the next waiting action the session can run now and waits for its
// answer; actions the camera cannot do are failed on the way. False if there
// is none to wait for.
bool CameraSession::send_next_action()
{
    namespace act = camera::action;
    while (!action_.active) {
        act::service::Job job;
        if (!act::service::take_next(list_ready_, camera_id_, job)) {
            return false;
        }
        const char *name = act::type_name(job.action.type);
        if (job.action.type == act::Type::delete_media &&
            (job.action.scope & act::kScopeCamera) == 0) {
            // The hub's own copies only (#461): the camera is not asked.
            const bool deleted = delete_hub_copies(job.action.media_id);
            save_session_catalog();
            act::service::finish(
                job.id, deleted ? act::Outcome{true, 0, ""}
                                : act::failure("the hub's files could not be "
                                               "deleted"));
            continue;
        }
        std::string problem = act::check_camera(job.action, device_info_);
        std::optional<act::Target> target;
        bool not_listed = false;
        if (problem.empty() && act::needs_media_list(job.action.type)) {
            target = act::find_target(catalog_, job.action.media_id);
            if (!target) {
                problem = "the camera does not list this file any more";
                not_listed = true;
            }
        }
        if (!problem.empty()) {
            if (not_listed && (job.action.scope & act::kScopeHub) != 0) {
                // Gone from the camera already: the hub's part still goes.
                delete_hub_copies(job.action.media_id);
                save_session_catalog();
            }
            act::service::finish(job.id, act::failure(problem));
            continue;
        }
        std::string fields = act::request_fields(job.action, target);
        if (job.action.type == act::Type::settings) {
            // As the app does: the request's own sequence number as "seq"
            // (docs/camera-settings.md). It is the number this command gets.
            fields += ",\"seq\":" + std::to_string(next_command_seq_);
            ESP_LOGI(TAG, "Running action %u: settings %s", (unsigned)job.id,
                     job.action.params.c_str());
        } else {
            ESP_LOGI(TAG, "Running action %u: %s", (unsigned)job.id, name);
        }
        if (!send_artemis_command(act::cmd_id(job.action.type), fields.c_str(),
                                  name)) {
            act::service::finish(
                job.id,
                act::failure("the command could not be sent to the camera"));
            continue;
        }
        action_.active = true;
        action_.id = job.id;
        action_.action = job.action;
        action_.sent_us = esp_timer_get_time();
    }
    return true;
}

// The session goes on with what follows the actions: the list, or the
// thumbnails.
void CameraSession::advance_after_actions()
{
    if (action_.active || send_next_action()) {
        return;
    }
    if (list_ready_) {
        after_list_actions();
    } else {
        begin_listing_phase();
    }
}

void CameraSession::handle_action_response(const std::string &json)
{
    namespace act = camera::action;
    act::Outcome outcome = act::interpret(action_.action.type, json);
    action_.active = false;
    const bool deleted_on_camera =
        outcome.done && action_.action.type == act::Type::delete_media;
    if (deleted_on_camera) {
        // Gone from the card: no thumbnail or download is wanted for it, and
        // its dir/num may belong to a new recording soon.
        catalog_.mark_gone(action_.action.media_id);
        if ((action_.action.scope & act::kScopeHub) != 0 &&
            !delete_hub_copies(action_.action.media_id)) {
            outcome = act::failure(
                "deleted on the camera, but the hub's files could not be "
                "deleted");
        }
        save_session_catalog();
    }
    act::service::finish(action_.id, outcome);
    if (outcome.done && action_.action.type == act::Type::settings &&
        !device_info_.dev_id.empty()) {
        // The camera took it: the stored copy (and with it the next check-in)
        // shows the new values until the next device info confirms them.
        std::vector<camera::settings::Value> change;
        if (camera::settings::decode_change(action_.action.params, change)) {
            camera::settings::apply_change(device_info_.settings, change);
            if (!camera::store::save_camera_info(device_info_,
                                                 (int64_t)time(NULL))) {
                ESP_LOGW(TAG,
                         "Could not store the new settings on the SD card");
            }
        }
    }
    advance_after_actions();
}

// An action the camera does not answer must not hold the session: it may or
// may not have been done, so the person is told that.
void CameraSession::check_action_timeout(int64_t now_us)
{
    if (!action_.active || now_us - action_.sent_us <= kActionAnswerTimeoutUs) {
        return;
    }
    ESP_LOGW(TAG, "No answer to action %u within 15 s", (unsigned)action_.id);
    action_.active = false;
    camera::action::service::finish(
        action_.id,
        camera::action::failure(
            "the camera did not answer within 15 s; it may have been done"));
    advance_after_actions();
}

}  // namespace camera::link
