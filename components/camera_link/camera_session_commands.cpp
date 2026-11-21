// The camera session's command channel: sending the ARTEMIS commands and
// handling the camera's answers (camera_session.hpp).

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

// Sends a token-authenticated ARTEMIS command over the session. `extra_fields`
// is inserted verbatim after cmdId (e.g. ",\"pageNo\":0"); it may be empty.
bool CameraSession::send_artemis_command(uint32_t cmd_id,
                                         const char *extra_fields,
                                         const char *context)
{
    const std::string json = "{\"cmdId\":" + std::to_string(cmd_id) +
                             extra_fields +
                             ",\"token\":" + std::to_string(token_) + "}";
    std::vector<uint8_t> packet = camera::artemis::build_request(
        command_cipher_, next_command_seq_++, json);
    const int64_t now_us = esp_timer_get_time();
    if (packet.empty()) {
        ESP_LOGE(TAG, "Could not send %s", context);
        return false;
    }
    // The camera acknowledges commands late while it streams media, so all
    // slots can still be in use although it answered the commands in them.
    // Dropping this one would leave the session waiting for an answer nobody
    // asked for; it goes out once a slot is free (send_deferred_command()).
    if (session_.outstanding_count() >=
        camera::pppp::Session::kMaxOutstanding) {
        if (deferred_command_.hold(std::move(packet), cmd_id, context,
                                   now_us)) {
            ESP_LOGW(TAG, "%s replaces a command that was still waiting",
                     context);
        }
        ESP_LOGW(TAG,
                 "No free DATA slot for %s; sending it once one is "
                 "acknowledged",
                 context);
        return true;
    }
    uint16_t index = 0;
    if (!session_.send_data(packet.data(), packet.size(), now_us, context,
                            &index)) {
        ESP_LOGE(TAG, "Could not send %s", context);
        return false;
    }
    ESP_LOGI(TAG, "Sent %s (cmdId %lu), index %u", context,
             (unsigned long)cmd_id, index);
    return true;
}

// Sends the command that waited for a free DATA slot, if there is one now.
// A send that fails with a free slot is repeated after a pause.
void CameraSession::send_deferred_command(int64_t now_us)
{
    if (!deferred_command_.due(now_us) ||
        session_.outstanding_count() >=
            camera::pppp::Session::kMaxOutstanding) {
        return;
    }
    const std::vector<uint8_t> &packet = deferred_command_.packet();
    uint16_t index = 0;
    if (!session_.send_data(packet.data(), packet.size(), now_us,
                            deferred_command_.context(), &index)) {
        ESP_LOGW(TAG, "Could not send %s; retrying",
                 deferred_command_.context());
        deferred_command_.note_send_failed(now_us);
        return;
    }
    ESP_LOGI(TAG, "Sent %s (cmdId %lu), index %u, after %lld ms without a slot",
             deferred_command_.context(),
             (unsigned long)deferred_command_.cmd_id(), index,
             (long long)(deferred_command_.held_us(now_us) / 1000));
    deferred_command_.clear();
}

// Decrypts a received ARTEMIS DATA message (reassembling fragments). On the
// login response it records the session token; the first command is sent only
// once the session has settled (see the heartbeat-ack path), mirroring the
// app's order. Other responses are logged as decrypted JSON, and the
// device-info reply triggers the media-list request.
void CameraSession::process_artemis_response(const SessionEvent &event)
{
    using namespace camera::artemis;

    const Reassembler::Status status =
        response_.feed(event.payload, event.payload_size);
    if (status == Reassembler::Status::incomplete) {
        return;
    }
    if (status == Reassembler::Status::invalid) {
        response_.reset();
        return;
    }

    Message message;
    const bool decoded = parse(command_cipher_, response_.message().data(),
                               response_.message().size(), message);
    response_.reset();
    if (!decoded) {
        ESP_LOGW(TAG, "Could not decode the ARTEMIS response");
        return;
    }

    int64_t cmd_id = -1;
    json_find_int(message.json, "cmdId", cmd_id);

    if (cmd_id == 0) {
        if (logged_in_) {
            return;
        }
        int64_t result = -1;
        int64_t token = 0;
        if (json_find_int(message.json, "result", result) && result == 0 &&
            json_find_int(message.json, "token", token)) {
            token_ = token;
            logged_in_ = true;
            ESP_LOGI(TAG, "ARTEMIS login succeeded; token %lld",
                     (long long)token);
        } else {
            std::string error_msg;
            json_find_string(message.json, "errorMsg", error_msg);
            ESP_LOGW(TAG, "ARTEMIS login rejected: %s",
                     json_redact(message.json).c_str());
            ESP_LOGW(TAG, "%s",
                     camera::artemis::login_rejected_hint(result, error_msg)
                         .c_str());
        }
        return;
    }

    // Never log a camera answer as it is: the device info (512) carries the
    // screen unlock code. Long answers (512, 768) go to DEBUG in full.
    {
        const std::string redacted = json_redact(message.json);
        if (redacted.size() <= kResponseLogInfoMax) {
            ESP_LOGI(TAG, "ARTEMIS response cmdId %lld: %s", (long long)cmd_id,
                     redacted.c_str());
        } else {
            ESP_LOGI(TAG, "ARTEMIS response cmdId %lld: %u bytes",
                     (long long)cmd_id, (unsigned)redacted.size());
            ESP_LOGD(TAG, "ARTEMIS response cmdId %lld: %s", (long long)cmd_id,
                     redacted.c_str());
        }
    }
    if (cmd_id != 525) {
        last_progress_us_ = esp_timer_get_time();
    }

    // The answer to the action the session waits for.
    if (action_.active) {
        const std::optional<camera::action::Type> type =
            camera::action::type_of_cmd(cmd_id);
        if (type && *type == action_.action.type) {
            handle_action_response(message.json);
            return;
        }
    }

    // The camera answers the playback request with the video's size, then
    // streams it. When it refuses the app's fileType, try the media list's.
    if (cmd_id == 769 && video_.active()) {
        if (video_.handle_playback_response(message.json,
                                            download_.queue[download_.index]) ==
            camera::link::PlaybackDownload::Response::refused) {
            finish_current_download(false, false);
        }
        return;
    }

    // The camera acknowledges a download request before streaming the file;
    // a non-zero result means it will not send it.
    if (cmd_id == 1285 && download_.active) {
        int64_t result = 0;
        if (json_find_int(message.json, "result", result) && result != 0) {
            ESP_LOGW(TAG, "Camera refused the download (result %lld)",
                     (long long)result);
            finish_current_download(false, false);
        }
        return;
    }

    // Parse the media list into the typed model and log a short summary, so
    // the model is exercised against the real response on hardware too.
    if (cmd_id == 768 && settings_.list_only) {
        handle_list_page(message.json);
        return;
    }
    if (cmd_id == 768) {
        handle_scan_page(message.json);
    }

    // Device info carries the camera identity (devId) and battery state. Use
    // the id to load that camera's catalog from the SD card, then request the
    // media list (as the app does; 768 needs paging parameters to answer).
    if (cmd_id == 512 && logged_in_ && !media_requested_) {
        DeviceInfo info;
        if (parse_device_info(message.json, info)) {
            device_info_ = info;
            camera_id_ = info.dev_id;
            // As the fixed-size field of the C interface held it: at most 47
            // characters.
            last_camera_id_ =
                camera::store::sanitize_camera_id(info.dev_id).substr(0, 47);
            if (info.bat_percent >= 0 && info.bat_percent <= 100) {
                battery_percent_ = (int)info.bat_percent;
            }
            // Only allowlisted fields (DeviceInfo), never the raw answer.
            ESP_LOGI(TAG,
                     "Camera '%s' model '%s' fw '%s' BLE '%s' name '%s': %s; "
                     "SD free %lld MB",
                     camera_id_.c_str(), info.model_name.c_str(),
                     info.fw_version.c_str(), info.ble_version.c_str(),
                     info.camera_name.c_str(),
                     device_state_summary(info).c_str(),
                     (long long)info.sd_free_mb);
            // A device without a clock takes the time of the camera: the
            // best it has, replaced by any better source later.
            int64_t camera_wall_clock = 0;
            if (!time_sync_clock_valid() &&
                json_find_int(message.json, "timeUTC", camera_wall_clock) &&
                camera_wall_clock > 0) {
                time_sync_set(
                    camera::camera_wall_clock_to_utc(camera_wall_clock),
                    TIME_SYNC_SOURCE_CAMERA);
            }
            if (!camera::store::save_camera_info(info, (int64_t)time(NULL))) {
                ESP_LOGW(TAG, "Could not store the camera info on the SD card");
            }
        } else {
            camera_id_ = "unknown";
            ESP_LOGW(TAG, "Could not parse device info; using camera id '%s'",
                     camera_id_.c_str());
        }
        media_requested_ = true;
        if (settings_.list_only) {
            listing_.reset();
            list_files_ = 0;
            request_list_page();
            return;
        }
        catalog_.clear();
        camera::store::load_catalog(camera_id_, catalog_);
        ESP_LOGI(TAG, "Loaded catalog: %u known items",
                 (unsigned)catalog_.size());

        list_ready_ = false;
        // Queued actions go first, so that a new photo is in the list that
        // follows; the list is requested when they are answered.
        if (send_next_action()) {
            return;
        }
        begin_listing_phase();
    }
}

}  // namespace camera::link
