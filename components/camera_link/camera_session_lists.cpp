// The camera session's media lists: the listing and the list of a scan
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

// --- Listing only ---

void CameraSession::report_list_line(const std::string &line)
{
    if (settings_.list_callback != nullptr) {
        settings_.list_callback(line.c_str());
    }
}

// Ends a listing with its final line.
void CameraSession::finish_list(const std::string &line)
{
    report_list_line(line);
    end_session_ = true;
}

// Records in camera.json what the first media-list page said about the
// camera's SD card, so the check-in can report it.
void CameraSession::record_sd_card(const camera::artemis::MediaList &list,
                                   camera::link::SdReport report)
{
    using camera::artemis::SdCard;
    if (report == camera::link::SdReport::none) {
        return;
    }
    const bool ok = report == camera::link::SdReport::present;
    std::string error;
    if (!ok) {
        error = list.error_msg.empty()
                    ? "getMediaListRet=" + std::to_string(list.list_ret)
                    : list.error_msg;
    }
    if (camera_id_.empty() ||
        !camera::store::set_camera_sd_card(
            camera_id_, ok ? SdCard::present : SdCard::missing, error)) {
        ESP_LOGW(TAG, "Could not record the SD card state of '%s'",
                 camera_id_.c_str());
    }
}

// Ends a listing that failed before it got an answer.
void CameraSession::fail_list(const std::string &reason)
{
    list_failed_ = true;
    finish_list("list failed " + reason);
}

void CameraSession::request_list_page()
{
    const std::string fields =
        camera::artemis::media_list_request_fields(listing_.page());
    if (!send_artemis_command(768, fields.c_str(), "ARTEMIS GetMediaList")) {
        fail_list("could not send the request");
    }
}

void CameraSession::handle_list_page(const std::string &json)
{
    using camera::link::ListingFlow;
    const ListingFlow::Step step = listing_.on_page(json);
    if (step.camera_error) {
        media_list_error_ = true;
    }
    record_sd_card(step.list, step.sd);
    list_files_ = listing_.files();
    for (size_t i = 0; i < step.lines.size(); ++i) {
        const bool last = i + 1 == step.lines.size();
        if (last && step.next != ListingFlow::Next::request_page) {
            if (step.next == ListingFlow::Next::failed) {
                list_failed_ = true;
            }
            finish_list(step.lines[i]);
        } else {
            report_list_line(step.lines[i]);
        }
    }
    if (step.next == ListingFlow::Next::request_page) {
        request_list_page();
    }
}

void CameraSession::request_scan_page()
{
    const std::string fields =
        camera::artemis::media_list_request_fields(scan_list_.page());
    if (!send_artemis_command(768, fields.c_str(), "ARTEMIS GetMediaList")) {
        ESP_LOGE(TAG, "Could not request media-list page %lld",
                 (long long)scan_list_.page());
        end_session_ = true;
    }
}

// All pages are merged: persist the catalog, then fetch the thumbnail of
// every listed item we do not have one for yet (newest first).
// `complete`: every page of the media list was read, so an item that was
// not listed is not on the camera's card.
void CameraSession::finish_scan_list(bool complete)
{
    // The web protocol view reads the "Media list parsed: N items" prefix.
    ESP_LOGI(TAG,
             "Media list parsed: %u items (%u photos, %u videos), %lld "
             "page(s)",
             (unsigned)(scan_list_.photos() + scan_list_.videos()),
             (unsigned)scan_list_.photos(), (unsigned)scan_list_.videos(),
             (long long)scan_list_.page());
    if (complete) {
        // Deleted on the camera (by hand or by the hub): the backend is told
        // at the next check-in, so the gallery drops them.
        const size_t removed = catalog_.mark_unlisted_gone();
        if (removed > 0) {
            ESP_LOGI(TAG, "%u recording(s) are no longer on the camera's card",
                     (unsigned)removed);
        }
        const size_t gone = catalog_.mark_unlisted_requests_unavailable();
        if (gone > 0) {
            ESP_LOGW(TAG,
                     "%u requested video(s) are no longer on the camera's "
                     "card",
                     (unsigned)gone);
        }
    }
    save_session_catalog();
    // The deletions run now, with the fresh list; then the thumbnails.
    list_ready_ = true;
    if (send_next_action()) {
        return;
    }
    if (camera::action::service::has_pending()) {
        ESP_LOGI(TAG, "Actions still wait, for another camera than %s",
                 camera_id_.c_str());
    }
    after_list_actions();
}

// One page of the media list of a scan (see ScanListFlow).
void CameraSession::handle_scan_page(const std::string &json)
{
    using camera::link::ScanListFlow;
    const int64_t page = scan_list_.page();
    const ScanListFlow::Step step = scan_list_.on_page(json, catalog_);
    record_sd_card(step.list, step.sd);
    switch (step.next) {
    case ScanListFlow::Next::end_session:
    case ScanListFlow::Next::partial:
        if (step.unparsed || step.camera_error) {
            ESP_LOGE(TAG,
                     "Media list page %lld failed (getMediaListRet %lld, "
                     "errorMsg '%s'%s)",
                     (long long)page, (long long)step.list.list_ret,
                     step.list.error_msg.c_str(),
                     step.unparsed ? ", no mediaFiles array" : "");
        }
        if (step.next == ScanListFlow::Next::end_session) {
            // Nothing to fetch: no thumbnail or download would be queued, so
            // the session would idle on heartbeats.
            ESP_LOGE(TAG, "Ending the camera session");
            media_list_error_ = !step.unparsed && step.camera_error;
            end_session_ = true;
            return;
        }
        if (step.page_limit) {
            ESP_LOGI(TAG, "Media list page %lld: %u items (%u new), cnt %lld",
                     (long long)page, (unsigned)step.list.files.size(),
                     (unsigned)step.added, (long long)step.list.count);
            ESP_LOGW(TAG,
                     "Media list has more than %d pages; older files are not "
                     "read in this session",
                     CONFIG_CAMERA_MEDIA_LIST_MAX_PAGES);
        }
        finish_scan_list(false);
        return;
    case ScanListFlow::Next::ignore:
        ESP_LOGW(TAG, "Ignoring media-list page %lld (expected %lld)",
                 (long long)step.list.page_no, (long long)page);
        return;
    case ScanListFlow::Next::complete:
    case ScanListFlow::Next::request_page:
        break;
    }
    ESP_LOGI(TAG, "Media list page %lld: %u items (%u new), cnt %lld",
             (long long)page, (unsigned)step.list.files.size(),
             (unsigned)step.added, (long long)step.list.count);
    if (step.next == ScanListFlow::Next::complete) {
        finish_scan_list(true);
    } else {
        request_scan_page();
    }
}

}  // namespace camera::link
