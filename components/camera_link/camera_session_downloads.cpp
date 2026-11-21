// The camera session's downloads of photos and videos (camera_session.hpp).

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

camera::artemis::MediaCatalog::VideoDownload
CameraSession::video_download() const
{
    if (settings_.all_videos &&
        kVideoDownload ==
            camera::artemis::MediaCatalog::VideoDownload::requested) {
        return camera::artemis::MediaCatalog::VideoDownload::missing;
    }
    return kVideoDownload;
}

void CameraSession::report_download_progress()
{
    if (settings_.progress_callback != nullptr) {
        settings_.progress_callback(
            static_cast<uint32_t>(download_.index),
            static_cast<uint32_t>(download_.stored),
            static_cast<uint32_t>(download_.queue.size()));
    }
}

// Builds the download queue once the thumbnails are done. Without enabled
// downloads, the session is closed right away.
void CameraSession::start_downloads()
{
    download_.queue = catalog_.download_queue(kDownloadPhotos, video_download(),
                                              CAMERA_VIDEO_MAX_PER_SESSION);
    download_.index = 0;
    download_.stored = 0;
    download_.stopped = false;
    if (kDownloadPhotos || kDownloadVideos) {
        ESP_LOGI(TAG, "%u files to download", (unsigned)download_.queue.size());
    }
    report_download_progress();
    request_current_download();
}

// Requests the current download queue item: a photo as a file (cmdId 1285),
// which streams back on the bulk channel for handle_download_frame(), or a
// video by playback (PlaybackDownload::start()). When the queue is exhausted,
// the session is closed.
void CameraSession::request_current_download()
{
    // Frames of a stopped video stream may still be on their way.
    if (video_.draining(esp_timer_get_time())) {
        return;
    }
    video_.clear_drain();
    while (download_.index < download_.queue.size()) {
        const camera::artemis::CatalogEntry &e =
            download_.queue[download_.index];
        if (e.file_type != 0) {
            if (video_.start(e, camera_id_, download_.index,
                             download_.queue.size())) {
                download_.active = true;
                download_.started_us = video_.started_us();
                download_.progress_us = download_.started_us;
                return;
            }
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            download_.index = download_.queue.size();
            break;
        }
        if (!media_file_.open(camera_id_, e.media_id, nullptr,
                              !camera::store::sd_writer_active())) {
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            download_.index = download_.queue.size();
            break;
        }
        const std::string extra =
            ",\"downloadReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
            ",\"dirNum\":" + std::to_string(e.dir_num) +
            ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
        if (send_artemis_command(1285, extra.c_str(), "ARTEMIS Download")) {
            ESP_LOGI(TAG, "Downloading %s dir %lld num %lld (%u of %u)",
                     e.file_type == 0 ? "photo" : "video", (long long)e.dir_num,
                     (long long)e.media_num, (unsigned)(download_.index + 1),
                     (unsigned)download_.queue.size());
            download_.stream.reset();
            download_.active = true;
            download_.started_us = esp_timer_get_time();
            download_.progress_us = download_.started_us;
            download_.logged_bytes = 0;
            return;
        }
        media_file_.abort();
        ++download_.index;
        report_download_progress();
    }
    video_.complete_pending(true);
    ESP_LOGI(TAG, "Downloads complete: %u of %u catalog items on the SD card",
             (unsigned)catalog_.count_with_flag(
                 camera::artemis::MediaCatalog::kFileDownloaded),
             (unsigned)catalog_.size());
    work_done_ = true;
}

// Ends the current download: on success the file gets its final name and the
// catalog records it; otherwise the partial file is removed and the item is
// tried again in a later session. Then the next item is requested, unless
// `stop_all` (the SD card failed).
void CameraSession::finish_current_download(bool stored, bool stop_all)
{
    using camera::artemis::MediaCatalog;
    const camera::artemis::CatalogEntry &e = download_.queue[download_.index];
    const bool was_video = video_.active();
    stop_all = stop_all || download_.stopped;
    // A video that ended normally is closed once the SD writer has written
    // it, while the next one is downloaded
    // (PlaybackDownload::complete_pending()).
    const bool deferred = video_.can_defer(stored);
    // Otherwise wait for the queued writes (or drop them if the file is not
    // kept).
    if (!deferred && !camera::store::sd_writer_flush(!stored) && stored) {
        stored = false;
        stop_all = true;
    }
    uint64_t video_bytes = 0;
    if (was_video) {
        stored = video_.finish(e, stored, deferred, &video_bytes);
    }
    camera::store::MediaFileWriter &file =
        was_video ? video_.video_file() : media_file_;
    if (deferred) {
        // Closed by PlaybackDownload::complete_pending().
    } else if (stored && file.finish(e.media_time)) {
        const int64_t elapsed_ms =
            (esp_timer_get_time() - download_.started_us) / 1000;
        const uint64_t bytes = was_video ? video_bytes
                                         : download_.stream.delivered_bytes() -
                                               download_.stream.prefix().size();
        ESP_LOGI(TAG, "Stored %s (%llu bytes in %lld ms, %llu KB/s)",
                 file.path().c_str(), (unsigned long long)bytes,
                 (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0 ? bytes / elapsed_ms : 0));
        catalog_.set_flag(e.media_id, MediaCatalog::kFileDownloaded);
        save_session_catalog();
        ++download_.stored;
        ++stored_items_;
    } else {
        file.abort();
        if (!(was_video && kDiscardVideoData)) {
            ESP_LOGW(TAG, "Download of dir %lld num %lld failed",
                     (long long)e.dir_num, (long long)e.media_num);
        }
        // A requested video is given up after a few failed downloads, so the
        // request is answered instead of waiting forever. A failure of the
        // SD card is not the video's.
        if (was_video && !kDiscardVideoData && !stop_all &&
            catalog_.has_flag(e.media_id, MediaCatalog::kVideoRequested)) {
            const uint32_t attempts =
                catalog_.count_failed_video_attempt(e.media_id);
            ESP_LOGW(TAG, "Requested video %lld: %u of %u attempts failed",
                     (long long)e.media_id, (unsigned)attempts,
                     (unsigned)MediaCatalog::kMaxVideoAttempts);
            save_session_catalog();
        }
    }
    download_.stream.reset();
    download_.active = false;
    bulk_.active = false;
    ++download_.index;
    if (stop_all) {
        ESP_LOGW(TAG, "SD card write failed; stopping downloads");
        download_.index = download_.queue.size();
    }
    report_download_progress();
    request_current_download();
}

// Handles one bulk frame of a running download: passes it to the stream,
// which writes the file in order, and acknowledges the frames it took.
// Returns whether the frame advanced the download (see handle_bulk_frame()).
bool CameraSession::handle_download_frame(
    int socket_fd, const struct sockaddr_in *camera_address, uint8_t subtype,
    uint16_t seq, const uint8_t *payload, size_t payload_len,
    bool is_record_start)
{
    using camera::artemis::BulkStream;
    if (!download_.stream.header_ready() && !is_record_start) {
        // Leftover of an earlier record: acknowledge it so the camera stops
        // resending it.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return false;
    }
    if (!download_.stream.header_ready()) {
        bulk_.subtype = subtype;
        bulk_.active = true;
    }

    bool sink_failed = false;
    const BulkStream::Sink sink = [this, &sink_failed](const uint8_t *data,
                                                       size_t size) {
        sink_failed = !camera::store::sd_writer_write(media_file_, data, size);
        return !sink_failed;
    };
    const uint64_t before = download_.stream.delivered_bytes();
    bool ack = false;
    const BulkStream::Status status =
        download_.stream.feed(seq, payload, payload_len, sink, ack);
    const bool progressed = download_.stream.progressed();
    if (ack) {
        queue_bulk_ack(socket_fd, camera_address, subtype, seq);
    }

    const uint64_t delivered = download_.stream.delivered_bytes();
    if (delivered > before) {
        download_.progress_us = esp_timer_get_time();
    }
    if (before == 0 && download_.stream.header_ready()) {
        ESP_LOGI(TAG, "Download record start: subtype 0x%02x, %u bytes",
                 (unsigned)subtype, (unsigned)download_.stream.record_len());
    }
    if (delivered - download_.logged_bytes >=
        CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES) {
        download_.logged_bytes = delivered;
        ESP_LOGI(TAG, "Download: %llu of %u bytes",
                 (unsigned long long)delivered,
                 (unsigned)download_.stream.record_len());
    }

    if (status == BulkStream::Status::invalid) {
        if (!sink_failed) {
            const size_t dump = payload_len < 24 ? payload_len : 24;
            ESP_LOGW(TAG, "Download record invalid; first bytes:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        }
        finish_current_download(false, sink_failed);
        return progressed;
    }
    if (status == BulkStream::Status::complete) {
        const auto &prefix = download_.stream.prefix();
        camera::artemis::MediaRecordHeader header;
        size_t data_offset = 0;
        size_t data_size = 0;
        const camera::artemis::CatalogEntry &e =
            download_.queue[download_.index];
        if (camera::artemis::parse_media_record(
                prefix.data(), prefix.size(), header, data_offset, data_size) &&
            (header.dir_num != e.dir_num || header.media_num != e.media_num)) {
            ESP_LOGW(TAG,
                     "Download record is for dir %lld num %lld, requested "
                     "dir %lld num %lld; discarding it",
                     (long long)header.dir_num, (long long)header.media_num,
                     (long long)e.dir_num, (long long)e.media_num);
            finish_current_download(false, false);
            return progressed;
        }
        finish_current_download(true, false);
    }
    return progressed;
}

void CameraSession::check_download_stall(int64_t now_us)
{
    video_.complete_pending(false);
    if (video_.drain_done(now_us) && !download_.active) {
        request_current_download();
        return;
    }
    if (video_.receiving()) {
        // The camera does not mark the end of the stream; it stops sending.
        if (video_.stream_ended(now_us)) {
            ESP_LOGI(TAG, "Video stream ended");
            finish_current_download(true, false);
        }
        return;
    }
    if (download_.active &&
        now_us - download_.progress_us >
            CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S * 1000000LL) {
        ESP_LOGW(TAG, "No download data for %d s; skipping the file",
                 CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S);
        finish_current_download(false, false);
    }
}

}  // namespace camera::link
