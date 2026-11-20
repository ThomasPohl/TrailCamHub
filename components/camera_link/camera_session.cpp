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

#define CAMERA_RUDP_RECEIVE_TIMEOUT_MS 500
#define CAMERA_RUDP_HEARTBEAT_INTERVAL_MS 1500
// Frames buffered ahead of a missing one while streaming a download (1 KB
// each); later frames are not acknowledged, so the camera resends them.
// The window is one block of up to 1472 bytes per frame, in PSRAM if there is
// one; without, internal RAM is too short for more. During a video playback
// the camera sends about 120 frames ahead of the first unacknowledged one, so
// the window must be larger than that: with 32 frames, most of them were
// dropped and sent again, which made the download about 20 times slower than
// playback (256 frames take about 377 KB of PSRAM per stream).
#if CONFIG_SPIRAM
#define CAMERA_DOWNLOAD_REORDER_FRAMES 256
#else
#define CAMERA_DOWNLOAD_REORDER_FRAMES 8
#endif
#define CAMERA_BULK_ACK_DELAY_US (1 * 1000LL)
// A thumbnail with no new frame for this long, or whose request brought no
// frame at all for this long, is requested again.
#define CAMERA_THUMBNAIL_STALL_US (3 * 1000 * 1000LL)
// Log a download's progress every this many bytes.
#define CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES (256u * 1024u)

#ifdef CONFIG_CAMERA_DOWNLOAD_PHOTOS
static const bool kDownloadPhotos = true;
#else
static const bool kDownloadPhotos = false;
#endif
// Videos are not offered by the file download (cmdId 1285): the camera
// acknowledges the request but sends nothing. They are fetched by playing
// them back (cmdId 769), which streams their H.264 and AAC frames.
#if defined(CONFIG_CAMERA_DOWNLOAD_VIDEOS) &&                                  \
    defined(CONFIG_CAMERA_VIDEO_ON_REQUEST)
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::requested;
#elif defined(CONFIG_CAMERA_DOWNLOAD_VIDEOS)
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::missing;
#else
static const camera::artemis::MediaCatalog::VideoDownload kVideoDownload =
    camera::artemis::MediaCatalog::VideoDownload::none;
#endif
static const bool kDownloadVideos =
    kVideoDownload != camera::artemis::MediaCatalog::VideoDownload::none;
// Camera answers up to this length are logged in full at INFO (after
// json_redact); longer ones only by size, and in full at DEBUG. With the log
// prefix this still fits a line of the SD log (SD_LOG_LINE_MAX, 256 bytes).
static constexpr size_t kResponseLogInfoMax = 200;
// Videos fetched in one session (0: no limit); each takes about twice its
// length.
#ifdef CONFIG_CAMERA_VIDEO_MAX_PER_SESSION
#define CAMERA_VIDEO_MAX_PER_SESSION CONFIG_CAMERA_VIDEO_MAX_PER_SESSION
#else
#define CAMERA_VIDEO_MAX_PER_SESSION 0
#endif
// Throughput test: receive and parse videos without writing them to the SD
// card (nothing is stored or marked as downloaded).
#ifdef CONFIG_CAMERA_VIDEO_DISCARD_DATA
static const bool kDiscardVideoData = true;
#else
static const bool kDiscardVideoData = false;
#endif
#ifdef CONFIG_CAMERA_THUMBNAIL_SERIAL_PREVIEW
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 1
#else
#define CAMERA_THUMBNAIL_SERIAL_PREVIEW 0
#endif
#ifndef CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
#define CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS                              \
    CAMERA_RUDP_HEARTBEAT_INTERVAL_MS
#endif
#ifndef CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S
#define CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S 20
#endif
#ifndef CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S
#define CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S 60
#endif
#if CONFIG_CAMERA_DOWNLOAD_PHOTOS || CONFIG_CAMERA_DOWNLOAD_VIDEOS
// Otherwise the session closes before a stalled download is given up and the
// next item requested (#106).
static_assert(CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S >
                  CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S,
              "CAMERA_SESSION_IDLE_TIMEOUT_S must be above "
              "CAMERA_DOWNLOAD_STALL_TIMEOUT_S");
#endif

static const char *TAG = "trailcamhub_protocol";
// The camera answers an action within a second or two; 518 took 0.9 s.
static constexpr int64_t kActionAnswerTimeoutUs = 15 * 1000 * 1000;
using camera::link::from_camera;
using camera::link::log_session_event;
using camera::link::send_udp_request;
using camera::link::set_receive_timeout;
using camera::link::stop_requested;
using SessionEvent = camera::pppp::Session::Event;

namespace camera::link
{

// Emits a completed JPEG over serial as Base64 (line-wrapped, with THUMB
// markers the web view can capture), guarded against oversized payloads.
static void
emit_thumbnail_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                    const camera::artemis::MediaRecordHeader &header)
{
    if (jpeg_size == 0 || jpeg_size > 512u * 1024u) {
        return;
    }
    const size_t b64_cap = ((jpeg_size + 2) / 3) * 4 + 1;
    uint8_t *b64 = static_cast<uint8_t *>(malloc(b64_cap));
    if (b64 == nullptr) {
        ESP_LOGW(TAG, "THUMB b64: out of memory for %u bytes",
                 (unsigned)jpeg_size);
        return;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, b64_cap, &olen, jpeg, jpeg_size) == 0) {
        ESP_LOGI(TAG, "THUMB JPEG dir=%lld num=%lld bytes=%u",
                 (long long)header.dir_num, (long long)header.media_num,
                 (unsigned)jpeg_size);
        const size_t line = 120;
        for (size_t off = 0; off < olen; off += line) {
            const size_t n = (olen - off < line) ? (olen - off) : line;
            ESP_LOGI(TAG, "THUMB b64:%.*s", (int)n,
                     reinterpret_cast<const char *>(b64) + off);
        }
        ESP_LOGI(TAG, "THUMB b64:END");
    }
    free(b64);
}

CameraSession::CameraSession(UdpTransport &transport,
                             camera::pppp::Session &session)
    : transport_(transport), session_(session),
      command_cipher_(camera::artemis::command_cipher()),
      catalog_saves_(16, 10 * 1000 * 1000),
      scan_list_(CONFIG_CAMERA_MEDIA_LIST_MAX_PAGES),
      download_stream_(camera::artemis::kMediaRecordHeaderSize,
                       CAMERA_DOWNLOAD_REORDER_FRAMES),
      // The video download calls back into the session through plain function
      // pointers (std::function would cost internal RAM).
      video_(
          command_cipher_, CAMERA_DOWNLOAD_REORDER_FRAMES,
          {this,
           [](void *self, uint32_t cmd_id, const char *extra,
              const char *context) {
               return static_cast<CameraSession *>(self)->send_artemis_command(
                   cmd_id, extra, context);
           },
           [](void *self, uint16_t seq) {
               auto *session = static_cast<CameraSession *>(self);
               session->queue_bulk_ack(session->ack_socket_,
                                       session->ack_camera_address_, 0x02, seq);
           },
           [](void *self, size_t size) {
               static_cast<CameraSession *>(self)->send_bulk_ack_before_waiting(
                   size);
           },
           [](void *self, int64_t now_us) {
               static_cast<CameraSession *>(self)->download_progress_us_ =
                   now_us;
           },
           [](void *self, bool stored, bool stop_all) {
               static_cast<CameraSession *>(self)->finish_current_download(
                   stored, stop_all);
           },
           [](void *self, const camera::artemis::CatalogEntry &e, bool stored,
              bool sd_failed) {
               auto *session = static_cast<CameraSession *>(self);
               if (stored) {
                   session->catalog_.set_flag(
                       e.media_id,
                       camera::artemis::MediaCatalog::kFileDownloaded);
                   session->save_session_catalog();
                   ++session->download_stored_;
                   ++session->stored_items_;
               }
               if (sd_failed) {
                   session->stop_downloads_ = true;
               }
               session->report_download_progress();
           }})
{
}

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

// Writes the catalog of the session's camera with every change so far.
void CameraSession::save_session_catalog()
{
    camera::store::save_catalog(camera_id_, catalog_);
    catalog_saves_.saved();
}

// Writes the catalog if thumbnails were stored since the last save.
void CameraSession::flush_catalog()
{
    if (catalog_saves_.pending() && !camera_id_.empty()) {
        save_session_catalog();
    }
    catalog_saves_.saved();
}

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

// Requests the thumbnail (cmdId 772) for the current queue item, if any. The
// reply streams back on the bulk channel; handle_bulk_frame() stores it and
// advances the queue. Returns false when the queue is exhausted or the
// request could not be sent; check_thumbnail_stall() then asks again.
bool CameraSession::request_current_thumbnail()
{
    if (thumbnails_.current() == nullptr) {
        thumbnail_requested_us_ = 0;
        ESP_LOGI(TAG, "Thumbnails complete: %u stored of %u catalog items",
                 (unsigned)catalog_.count_with_flag(
                     camera::artemis::MediaCatalog::kThumbPresent),
                 (unsigned)catalog_.size());
        flush_catalog();
        start_downloads();
        return false;
    }
    const std::string extra = thumbnails_.request_fields();
    thumbnail_requested_us_ = esp_timer_get_time();
    return send_artemis_command(772, extra.c_str(), "ARTEMIS GetThumbnail");
}

// Sends one ACK for the bulk frames received since the last one.
void CameraSession::send_bulk_ack(int socket_fd,
                                  const struct sockaddr_in *camera_address)
{
    if (bulk_ack_pending_ == 0) {
        return;
    }
    camera::ArtemisBulkAckRequest ack;
    const bool built =
        ack.build(bulk_ack_subtype_, bulk_ack_seqs_, bulk_ack_pending_);
    video_.note_ack_sent(bulk_ack_subtype_, bulk_ack_seqs_, bulk_ack_pending_);
    bulk_ack_pending_ = 0;
    if (built && send_udp_request(socket_fd, camera_address, "BULK ACK", ack) !=
                     ESP_OK) {
        video_.note_ack_failure();
    }
}

// Sends the pending ACK now if writing `size` bytes is about to wait for the
// SD card: the camera would otherwise take the frames received meanwhile for
// lost and send them again.
void CameraSession::send_bulk_ack_before_waiting(size_t size)
{
    if (bulk_ack_pending_ > 0 && ack_socket_ >= 0 &&
        !camera::store::sd_writer_has_room(size)) {
        send_bulk_ack(ack_socket_, ack_camera_address_);
    }
}

// Adds a frame to the next ACK; sends it once it is full.
void CameraSession::queue_bulk_ack(int socket_fd,
                                   const struct sockaddr_in *camera_address,
                                   uint8_t subtype, uint16_t seq)
{
    if (bulk_ack_pending_ > 0 && subtype != bulk_ack_subtype_) {
        send_bulk_ack(socket_fd, camera_address);
    }
    bulk_ack_subtype_ = subtype;
    bulk_ack_seqs_[bulk_ack_pending_++] = seq;
    if (bulk_ack_pending_ >= kBulkAckMax) {
        send_bulk_ack(socket_fd, camera_address);
    }
}

void CameraSession::report_download_progress()
{
    if (settings_.progress_callback != nullptr) {
        settings_.progress_callback(
            static_cast<uint32_t>(download_index_),
            static_cast<uint32_t>(download_stored_),
            static_cast<uint32_t>(download_queue_.size()));
    }
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
    while (download_index_ < download_queue_.size()) {
        const camera::artemis::CatalogEntry &e =
            download_queue_[download_index_];
        if (e.file_type != 0) {
            if (video_.start(e, camera_id_, download_index_,
                             download_queue_.size())) {
                download_active_ = true;
                download_started_us_ = video_.started_us();
                download_progress_us_ = download_started_us_;
                return;
            }
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            download_index_ = download_queue_.size();
            break;
        }
        if (!media_file_.open(camera_id_, e.media_id, nullptr,
                              !camera::store::sd_writer_active())) {
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            download_index_ = download_queue_.size();
            break;
        }
        const std::string extra =
            ",\"downloadReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
            ",\"dirNum\":" + std::to_string(e.dir_num) +
            ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
        if (send_artemis_command(1285, extra.c_str(), "ARTEMIS Download")) {
            ESP_LOGI(TAG, "Downloading %s dir %lld num %lld (%u of %u)",
                     e.file_type == 0 ? "photo" : "video", (long long)e.dir_num,
                     (long long)e.media_num, (unsigned)(download_index_ + 1),
                     (unsigned)download_queue_.size());
            download_stream_.reset();
            download_active_ = true;
            download_started_us_ = esp_timer_get_time();
            download_progress_us_ = download_started_us_;
            download_logged_bytes_ = 0;
            return;
        }
        media_file_.abort();
        ++download_index_;
        report_download_progress();
    }
    video_.complete_pending(true);
    ESP_LOGI(TAG, "Downloads complete: %u of %u catalog items on the SD card",
             (unsigned)catalog_.count_with_flag(
                 camera::artemis::MediaCatalog::kFileDownloaded),
             (unsigned)catalog_.size());
    work_done_ = true;
}

// Builds the download queue once the thumbnails are done. Without enabled
// downloads, the session is closed right away.
void CameraSession::start_downloads()
{
    download_queue_ = catalog_.download_queue(kDownloadPhotos, video_download(),
                                              CAMERA_VIDEO_MAX_PER_SESSION);
    download_index_ = 0;
    download_stored_ = 0;
    stop_downloads_ = false;
    if (kDownloadPhotos || kDownloadVideos) {
        ESP_LOGI(TAG, "%u files to download", (unsigned)download_queue_.size());
    }
    report_download_progress();
    request_current_download();
}

// Ends the current download: on success the file gets its final name and the
// catalog records it; otherwise the partial file is removed and the item is
// tried again in a later session. Then the next item is requested, unless
// `stop_all` (the SD card failed).
void CameraSession::finish_current_download(bool stored, bool stop_all)
{
    using camera::artemis::MediaCatalog;
    const camera::artemis::CatalogEntry &e = download_queue_[download_index_];
    const bool was_video = video_.active();
    stop_all = stop_all || stop_downloads_;
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
            (esp_timer_get_time() - download_started_us_) / 1000;
        const uint64_t bytes = was_video ? video_bytes
                                         : download_stream_.delivered_bytes() -
                                               download_stream_.prefix().size();
        ESP_LOGI(TAG, "Stored %s (%llu bytes in %lld ms, %llu KB/s)",
                 file.path().c_str(), (unsigned long long)bytes,
                 (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0 ? bytes / elapsed_ms : 0));
        catalog_.set_flag(e.media_id, MediaCatalog::kFileDownloaded);
        save_session_catalog();
        ++download_stored_;
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
    download_stream_.reset();
    download_active_ = false;
    bulk_active_ = false;
    ++download_index_;
    if (stop_all) {
        ESP_LOGW(TAG, "SD card write failed; stopping downloads");
        download_index_ = download_queue_.size();
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
    if (!download_stream_.header_ready() && !is_record_start) {
        // Leftover of an earlier record: acknowledge it so the camera stops
        // resending it.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return false;
    }
    if (!download_stream_.header_ready()) {
        bulk_subtype_ = subtype;
        bulk_active_ = true;
    }

    bool sink_failed = false;
    const BulkStream::Sink sink = [this, &sink_failed](const uint8_t *data,
                                                       size_t size) {
        sink_failed = !camera::store::sd_writer_write(media_file_, data, size);
        return !sink_failed;
    };
    const uint64_t before = download_stream_.delivered_bytes();
    bool ack = false;
    const BulkStream::Status status =
        download_stream_.feed(seq, payload, payload_len, sink, ack);
    const bool progressed = download_stream_.progressed();
    if (ack) {
        queue_bulk_ack(socket_fd, camera_address, subtype, seq);
    }

    const uint64_t delivered = download_stream_.delivered_bytes();
    if (delivered > before) {
        download_progress_us_ = esp_timer_get_time();
    }
    if (before == 0 && download_stream_.header_ready()) {
        ESP_LOGI(TAG, "Download record start: subtype 0x%02x, %u bytes",
                 (unsigned)subtype, (unsigned)download_stream_.record_len());
    }
    if (delivered - download_logged_bytes_ >=
        CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES) {
        download_logged_bytes_ = delivered;
        ESP_LOGI(TAG, "Download: %llu of %u bytes",
                 (unsigned long long)delivered,
                 (unsigned)download_stream_.record_len());
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
        const auto &prefix = download_stream_.prefix();
        camera::artemis::MediaRecordHeader header;
        size_t data_offset = 0;
        size_t data_size = 0;
        const camera::artemis::CatalogEntry &e =
            download_queue_[download_index_];
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

// A thumbnail record that stops arriving: the heartbeat is paused during a
// bulk transfer, so without this the camera would end the session. Request
// the thumbnail again, or skip it after a second stall. The same goes for a
// request that brings no record at all (it could not be sent, the camera lost
// it, or the record was rejected): nothing else would ask again, and the
// session would idle on heartbeats until its watchdog.
void CameraSession::check_thumbnail_stall(int64_t now_us)
{
    const char *const next =
        thumbnails_.retrying() ? "skipping it" : "requesting it again";
    if (bulk_active_) {
        if (bulk_subtype_ != 0x04 ||
            now_us - bulk_last_frame_us_ < CAMERA_THUMBNAIL_STALL_US) {
            return;
        }
        ESP_LOGW(TAG, "Thumbnail stalled at %u of %u frames; %s",
                 (unsigned)bulk_.received_frames(),
                 (unsigned)bulk_.total_frames(), next);
    } else {
        // Counted from the request, or from the last frame of a record that
        // was rejected after it.
        if (thumbnail_requested_us_ == 0 ||
            now_us - std::max(thumbnail_requested_us_, bulk_last_frame_us_) <
                CAMERA_THUMBNAIL_STALL_US) {
            return;
        }
        ESP_LOGW(TAG, "No thumbnail data %d s after the request; %s",
                 (int)(CAMERA_THUMBNAIL_STALL_US / 1000000LL), next);
    }
    bulk_.reset();
    bulk_active_ = false;
    thumbnails_.on_stall();
    request_current_thumbnail();
}

void CameraSession::check_download_stall(int64_t now_us)
{
    video_.complete_pending(false);
    if (video_.drain_done(now_us) && !download_active_) {
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
    if (download_active_ &&
        now_us - download_progress_us_ >
            CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S * 1000000LL) {
        ESP_LOGW(TAG, "No download data for %d s; skipping the file",
                 CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S);
        finish_current_download(false, false);
    }
}

// Handles one bulk-stream frame (D0/D1 subtype 0x03/0x04): feeds the
// reassembler, acknowledges the window so the camera advances to the next
// frames, and on completion extracts and emits the JPEG. Returns whether the
// frame advanced the work: a new frame of the current record, download or
// video. Stray frames between records and repeats do not count, so a camera
// that only resends them does not keep the session alive (watchdog).
bool CameraSession::handle_bulk_frame(int socket_fd,
                                      const struct sockaddr_in *camera_address,
                                      const uint8_t *packet, size_t size)
{
    using namespace camera::artemis;
    static const uint8_t kSig[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    // Bytes of a side-channel frame shown in the log.
    static const size_t kSideChannelDumpMax = 96;
    const uint8_t subtype = packet[5];
    const uint16_t seq = static_cast<uint16_t>((packet[6] << 8) | packet[7]);
    const uint8_t *payload = packet + 8;
    const size_t payload_len = size - 8;
    const bool is_record_start =
        payload_len >= 20 && memcmp(payload, kSig, sizeof(kSig)) == 0;

    // During a video download the stream is on subtype 0x02; the other
    // subtypes are ignored.
    if (video_.active()) {
        if (subtype == 0x02) {
            return video_.handle_frame(seq, payload, payload_len);
        }
        // Leftovers of the thumbnails: acknowledge them, so that the camera
        // does not send them again and again, taking airtime from the video.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        video_.note_other_subtype_frame(subtype, seq, payload, payload_len);
        return false;
    }
    // Leftovers of a stopped video stream.
    if (subtype == 0x02) {
        video_.note_leftover_frame();
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return false;
    }
    // Records the camera sends on its own: acknowledge them, so that it does
    // not send them again and again, and leave the record being received
    // alone. They are no progress of the session.
    if (is_bulk_side_channel(subtype, download_active_)) {
        camera::ArtemisBulkAckRequest side_ack;
        if (side_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address,
                             "BULK ACK (side channel)", side_ack);
        }
        if (side_channel_.note(subtype, seq)) {
            // Diagnostics until the records are understood: every frame
            // once, with its content.
            const size_t dump = payload_len < kSideChannelDumpMax
                                    ? payload_len
                                    : kSideChannelDumpMax;
            ESP_LOGI(TAG,
                     "BULK side channel 0x%02x seq %u: %u payload bytes "
                     "(frame %u of this session)%s",
                     (unsigned)subtype, (unsigned)seq, (unsigned)payload_len,
                     (unsigned)side_channel_.frames(),
                     is_record_start ? ", record start" : "");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_INFO);
        }
        return false;
    }
    // Thumbnails keep subtype 0x04; any other subtype during a download is
    // the requested file (0x03 for photos).
    if (download_active_ && subtype != 0x04) {
        return handle_download_frame(socket_fd, camera_address, subtype, seq,
                                     payload, payload_len, is_record_start);
    }

    if (!bulk_active_ || subtype != bulk_subtype_) {
        // A record only begins on its header-bearing (ARTEMIS) frame. Any
        // other frame received between records is a retransmit of a finished
        // record or a reordered straggler: acknowledge it so the camera stops
        // resending and moves on, but do not start a record on it.
        if (!is_record_start) {
            camera::ArtemisBulkAckRequest stray_ack;
            if (stray_ack.build(subtype, &seq, 1)) {
                send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                                 stray_ack);
            }
            ESP_LOGD(TAG,
                     "BULK subtype 0x%02x seq %u: stray frame ignored (%u B)",
                     (unsigned)subtype, (unsigned)seq, (unsigned)payload_len);
            return false;
        }
        // The camera may resend the header frame of a record that is already
        // complete; a record starting before the end of the last one is such
        // a repeat, not the next record.
        if (bulk_next_seq_valid_ && subtype == bulk_subtype_ &&
            static_cast<int16_t>(seq - bulk_next_seq_) < 0) {
            camera::ArtemisBulkAckRequest stray_ack;
            if (stray_ack.build(subtype, &seq, 1)) {
                send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                                 stray_ack);
            }
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x seq %u: repeated header of a "
                     "finished record ignored (next record from seq %u)",
                     (unsigned)subtype, (unsigned)seq,
                     (unsigned)bulk_next_seq_);
            return false;
        }
        bulk_.reset();
        bulk_subtype_ = subtype;
        bulk_active_ = true;
    }

    bulk_last_frame_us_ = esp_timer_get_time();
    const bool was_header = bulk_.header_ready();
    const size_t before = bulk_.received_frames();
    const BulkReassembler::Status status =
        bulk_.feed(seq, payload, payload_len);
    // The reassembler ignores repeated frames; only a new one counts.
    const bool progressed = bulk_.received_frames() > before;

    // Acknowledge the frame so the camera keeps streaming.
    queue_bulk_ack(socket_fd, camera_address, subtype, seq);

    // Log the record boundary so successive records can be told apart.
    if (!was_header && bulk_.header_ready()) {
        ESP_LOGI(TAG,
                 "BULK record start: subtype 0x%02x base seq %u, ver %u typ %u "
                 "len %u (%u frames)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)bulk_.version(),
                 (unsigned)bulk_.type(), (unsigned)bulk_.record_len(),
                 (unsigned)bulk_.total_frames());
    }

    const size_t frames = bulk_.received_frames();
    if (frames > before && (frames == 1 || frames % 8 == 0)) {
        ESP_LOGI(TAG, "BULK subtype 0x%02x seq %u (%u/%u frames, %u bytes)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)frames,
                 (unsigned)bulk_.total_frames(),
                 (unsigned)bulk_.received_bytes());
    }

    if (status == BulkReassembler::Status::invalid) {
        // Diagnostics: say why the frame was rejected and show its first
        // bytes, so a non-header frame arriving as a record start (reordering,
        // a straggler, or an unexpected framing) is visible in the log.
        const size_t dump = payload_len < 24 ? payload_len : 24;
        ESP_LOGW(TAG,
                 "BULK subtype 0x%02x seq %u: invalid record (%s), "
                 "%u payload bytes; first bytes:",
                 (unsigned)subtype, (unsigned)seq,
                 BulkReassembler::reason_name(bulk_.invalid_reason()),
                 (unsigned)payload_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        if (bulk_.invalid_reason() ==
            BulkReassembler::InvalidReason::no_memory) {
            // The thumbnail is skipped; the stall handling asks for it again
            // or moves on.
            ESP_LOGW(
                TAG,
                "No %u B block for the thumbnail record (largest free "
                "block %u B, free heap %u B)",
                (unsigned)bulk_.total_bytes(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        }
        bulk_.reset();
        bulk_active_ = false;
        return false;
    }
    if (status == BulkReassembler::Status::complete) {
        bulk_next_seq_ =
            static_cast<uint16_t>(bulk_.base_seq() + bulk_.total_frames());
        bulk_next_seq_valid_ = true;
        MediaRecordHeader header;
        size_t jpeg_offset = 0;
        size_t jpeg_size = 0;
        const bool parsed =
            parse_media_record(bulk_.payload(), bulk_.payload_size(), header,
                               jpeg_offset, jpeg_size);
        if (parsed) {
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x complete: %u record bytes, "
                     "%u JPEG bytes (dir %lld num %lld)",
                     (unsigned)subtype, (unsigned)bulk_.received_bytes(),
                     (unsigned)jpeg_size, (long long)header.dir_num,
                     (long long)header.media_num);
        }

        if (parsed && subtype == 0x04) {
            // Thumbnail: store it on the SD card and record it in the catalog.
            // Date the file by the capture time from the media list.
            const uint8_t *jpeg = bulk_.payload() + jpeg_offset;
            // The answer names dir/num, which is unique among the items of
            // the current media list (only those are requested).
            const CatalogEntry *entry =
                catalog_.find_listed(header.dir_num, header.media_num);
            if (entry == nullptr) {
                ESP_LOGW(TAG,
                         "Thumbnail dir %lld num %lld is not in the media "
                         "list; not stored",
                         (long long)header.dir_num,
                         (long long)header.media_num);
            } else {
                const int64_t media_id = entry->media_id;
                if (header.media_id == 0) {
                    ESP_LOGD(
                        TAG, "Thumbnail dir %lld num %lld carries no media id",
                        (long long)header.dir_num, (long long)header.media_num);
                } else if (header.media_id != media_id) {
                    ESP_LOGW(TAG,
                             "Thumbnail dir %lld num %lld: record id %lld, "
                             "list mediaId %lld",
                             (long long)header.dir_num,
                             (long long)header.media_num,
                             (long long)header.media_id, (long long)media_id);
                }
                if (camera::store::write_thumbnail(camera_id_, media_id, jpeg,
                                                   jpeg_size,
                                                   entry->media_time)) {
                    catalog_.set_flag(media_id, MediaCatalog::kThumbPresent);
                    if (catalog_saves_.note_change(esp_timer_get_time())) {
                        save_session_catalog();
                    }
                    ++stored_items_;
                } else {
                    ESP_LOGW(TAG, "Could not store thumbnail dir %lld num %lld",
                             (long long)header.dir_num,
                             (long long)header.media_num);
                }
            }
            // Emit the first thumbnail of the cycle over serial (for the web
            // live view); the rest just go to the SD card to avoid flooding.
            if (CAMERA_THUMBNAIL_SERIAL_PREVIEW && !thumb_emitted_) {
                emit_thumbnail_jpeg(jpeg, jpeg_size, header);
                thumb_emitted_ = true;
            }
        }

        bulk_.reset();
        bulk_active_ = false;

        if (subtype == 0x04) {
            thumbnails_.advance();
            request_current_thumbnail();
        }
    }
    return progressed;
}

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

// Sends the next waiting action the session can run now and waits for its
// answer; actions the camera cannot do are failed on the way. False if there
// is none to wait for.
bool CameraSession::send_next_action()
{
    namespace act = camera::action;
    while (!action_active_) {
        act::service::Job job;
        if (!act::service::take_next(list_ready_, camera_id_, job)) {
            return false;
        }
        const char *name = act::type_name(job.action.type);
        std::string problem = act::check_camera(job.action.type, device_info_);
        std::optional<act::Target> target;
        if (problem.empty() && act::needs_media_list(job.action.type)) {
            target = act::find_target(catalog_, job.action.media_id);
            if (!target) {
                problem = "the camera does not list this file any more";
            }
        }
        if (!problem.empty()) {
            act::service::finish(job.id, act::failure(problem));
            continue;
        }
        const std::string fields = act::request_fields(job.action, target);
        ESP_LOGI(TAG, "Running action %u: %s", (unsigned)job.id, name);
        if (!send_artemis_command(act::cmd_id(job.action.type), fields.c_str(),
                                  name)) {
            act::service::finish(
                job.id,
                act::failure("the command could not be sent to the camera"));
            continue;
        }
        action_active_ = true;
        action_id_ = job.id;
        action_ = job.action;
        action_sent_us_ = esp_timer_get_time();
    }
    return true;
}

// The session goes on with what follows the actions: the list, or the
// thumbnails.
void CameraSession::advance_after_actions()
{
    if (action_active_ || send_next_action()) {
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
    const act::Outcome outcome = act::interpret(action_.type, json);
    action_active_ = false;
    act::service::finish(action_id_, outcome);
    if (outcome.done && action_.type == act::Type::delete_media) {
        // Gone from the card: no thumbnail or download is wanted for it, and
        // its dir/num may belong to a new recording soon.
        catalog_.mark_gone(action_.media_id);
        save_session_catalog();
    }
    advance_after_actions();
}

// An action the camera does not answer must not hold the session: it may or
// may not have been done, so the person is told that.
void CameraSession::check_action_timeout(int64_t now_us)
{
    if (!action_active_ || now_us - action_sent_us_ <= kActionAnswerTimeoutUs) {
        return;
    }
    ESP_LOGW(TAG, "No answer to action %u within 15 s", (unsigned)action_id_);
    action_active_ = false;
    camera::action::service::finish(
        action_id_,
        camera::action::failure(
            "the camera did not answer within 15 s; it may have been done"));
    advance_after_actions();
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

// Fetches the thumbnail of every listed item we do not have one for yet
// (newest first), then the downloads.
void CameraSession::begin_thumbnails()
{
    thumbnails_.start(catalog_.missing_thumbnails());
    thumb_emitted_ = false;
    ESP_LOGI(TAG, "Catalog %s: %u items (%u new), %u thumbnails to fetch",
             camera_id_.c_str(), (unsigned)catalog_.size(),
             (unsigned)scan_list_.added(), (unsigned)thumbnails_.size());
    request_current_thumbnail();
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
    if (action_active_) {
        const std::optional<camera::action::Type> type =
            camera::action::type_of_cmd(cmd_id);
        if (type && *type == action_.type) {
            handle_action_response(message.json);
            return;
        }
    }

    // The camera answers the playback request with the video's size, then
    // streams it. When it refuses the app's fileType, try the media list's.
    if (cmd_id == 769 && video_.active()) {
        if (video_.handle_playback_response(message.json,
                                            download_queue_[download_index_]) ==
            camera::link::PlaybackDownload::Response::refused) {
            finish_current_download(false, false);
        }
        return;
    }

    // The camera acknowledges a download request before streaming the file;
    // a non-zero result means it will not send it.
    if (cmd_id == 1285 && download_active_) {
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

// Keeps the ARTEMIS session alive with heartbeats (one at a time, sent
// after the previous one was acknowledged) and logs everything received
// until the camera closes the session or a stop is requested.
esp_err_t
CameraSession::run_artemis_session(int socket_fd,
                                   const struct sockaddr_in *camera_address,
                                   int64_t discovery_started_us)
{
    camera::artemis::Heartbeat heartbeat;
    uint16_t heartbeat_index = 0;
    bool heartbeat_pending = false;
    ack_socket_ = socket_fd;
    ack_camera_address_ = camera_address;
    int64_t last_heartbeat_us = 0;
    int64_t heartbeat_interval_us = CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL;
    int64_t last_receive_us = 0;
    response_.reset();
    logged_in_ = false;
    token_ = 0;
    devinfo_requested_ = false;
    media_requested_ = false;
    next_command_seq_ = 2;
    camera_id_.clear();
    catalog_.clear();
    thumbnails_.release();
    thumbnail_requested_us_ = 0;
    deferred_command_.clear();
    thumb_emitted_ = false;
    scan_list_.reset();
    work_done_ = false;
    end_session_ = false;
    last_progress_us_ = esp_timer_get_time();
    bulk_.reset();
    bulk_subtype_ = 0;
    bulk_ack_pending_ = 0;
    bulk_active_ = false;
    bulk_next_seq_valid_ = false;
    side_channel_.reset();
    download_queue_.clear();
    download_index_ = 0;
    download_active_ = false;
    download_stream_.reset();
    video_.begin_session();
    device_info_ = camera::artemis::DeviceInfo{};
    action_active_ = false;
    list_ready_ = false;
    ESP_LOGI(TAG, "Starting ARTEMIS heartbeat loop");

    while (!stop_requested()) {
        // All requested work is done: close the camera session cleanly
        // (PPPP/CS2 CLOSE) and finish, rather than idling on heartbeats.
        if (work_done_ || end_session_) {
            const camera::ControlRequest close(camera::PacketType::close);
            send_udp_request(socket_fd, camera_address, "CLOSE", close);
            ESP_LOGI(TAG, "%s; closed the camera session",
                     work_done_ ? "Work complete" : "Nothing more to do");
            // A listing ends this way when it is done. A scan that could not
            // do its work (e.g. no SD card in the camera), or a listing that
            // failed, did not read the camera.
            const bool ok =
                work_done_ || (settings_.list_only && !list_failed_);
            return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        int64_t now_us = esp_timer_get_time();
        // A request the camera never answers (or an answer we do not
        // handle) would keep the session, and the camera's Wi-Fi, alive on
        // heartbeats until its battery is empty.
        if (now_us - last_progress_us_ >
            CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S * 1000000LL) {
            ESP_LOGE(TAG,
                     "No answer or data from the camera for %d s; closing "
                     "the camera session",
                     CONFIG_CAMERA_SESSION_IDLE_TIMEOUT_S);
            const camera::ControlRequest close(camera::PacketType::close);
            send_udp_request(socket_fd, camera_address, "CLOSE", close);
            return ESP_ERR_TIMEOUT;
        }
        check_download_stall(now_us);
        check_thumbnail_stall(now_us);
        check_action_timeout(now_us);
        // Before the heartbeat, so that it does not take the freed slot.
        send_deferred_command(now_us);
        // A video download may use its own interval: the camera streamed about
        // two video frames per heartbeat, as if it were held back by them.
        heartbeat_interval_us =
            (video_.active() ? CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
                             : CAMERA_RUDP_HEARTBEAT_INTERVAL_MS) *
            1000LL;
        // Pause the session heartbeat during a bulk transfer: it would not be
        // acknowledged while the camera is streaming, and its retransmissions
        // only compete with our frame ACKs on the uplink.
        if (!bulk_active_ && !heartbeat_pending &&
            (last_heartbeat_us == 0 ||
             now_us - last_heartbeat_us >= heartbeat_interval_us)) {
            // The session copies the packet (for retransmissions).
            const std::vector<uint8_t> body = heartbeat.next(command_cipher_);
            if (!body.empty() &&
                session_.send_data(body.data(), body.size(), now_us,
                                   "ARTEMIS heartbeat", &heartbeat_index)) {
                heartbeat_pending = true;
                last_heartbeat_us = now_us;
                video_.note_heartbeat();
                ESP_LOGI(TAG,
                         "ARTEMIS heartbeat command %u, index %u awaiting ACK",
                         heartbeat.count(), heartbeat_index);
            } else if (!session_.transport_failed()) {
                if (session_.outstanding_count() <
                    camera::pppp::Session::kMaxOutstanding) {
                    // A send error, not a full window: try again after the
                    // heartbeat interval instead of in a tight loop.
                    last_heartbeat_us = now_us;
                    ESP_LOGW(TAG, "Could not send the heartbeat; retrying");
                } else {
                    ESP_LOGW(TAG, "No free DATA slot for the heartbeat");
                }
            }
        }
        size_t retransmitted = bulk_active_ ? 0 : session_.poll(now_us);
        if (retransmitted > 0) {
            ESP_LOGW(TAG, "Retransmitted %u unacknowledged DATA packet(s)",
                     (unsigned)retransmitted);
        }
        if (session_.transport_failed()) {
            return ESP_FAIL;
        }

        // Wake up in time for the next retransmission or heartbeat. During a
        // bulk transfer both are paused, so just block for the full interval
        // and stay responsive to incoming frames.
        int64_t timeout_us = CAMERA_RUDP_RECEIVE_TIMEOUT_MS * 1000LL;
        if (!bulk_active_) {
            timeout_us = session_.time_until_retransmit_us(now_us, timeout_us);
            timeout_us =
                deferred_command_.time_until_retry_us(now_us, timeout_us);
            if (!heartbeat_pending) {
                int64_t heartbeat_due_us =
                    heartbeat_interval_us - (now_us - last_heartbeat_us);
                if (heartbeat_due_us < timeout_us) {
                    timeout_us = heartbeat_due_us;
                }
            }
        }
        // Send the pending bulk ACK once no frame follows shortly.
        if (bulk_ack_pending_ > 0 && timeout_us > CAMERA_BULK_ACK_DELAY_US) {
            timeout_us = CAMERA_BULK_ACK_DELAY_US;
        }
        esp_err_t timeout_error =
            set_receive_timeout(socket_fd, timeout_us, "ARTEMIS");
        if (timeout_error != ESP_OK) {
            return timeout_error;
        }

        // Static (not on the task stack): a fragment datagram is ~1.6 KB, and
        // only the single camera session task uses it. In internal RAM: every
        // received frame, duplicates included, is written here and read
        // again, which costs noticeably more CPU time in PSRAM.
        static uint8_t packet[camera::ArtemisRudpPacket::kMaxPacketSize];
        struct sockaddr_in source = {};
        if (video_.active() && last_receive_us != 0) {
            video_.note_loop_busy(esp_timer_get_time() - last_receive_us);
        }
        ssize_t received = camera_udp_receive_packet(
            socket_fd, packet, sizeof(packet), &source, "ARTEMIS");
        last_receive_us = esp_timer_get_time();
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_bulk_ack(socket_fd, camera_address);
                continue;
            }
            ESP_LOGE(TAG, "ARTEMIS packet receive failed: errno %d", errno);
            return ESP_FAIL;
        }
        if (!from_camera(&source, camera_address)) {
            ESP_LOGW(TAG, "Ignoring UDP packet from unexpected endpoint");
            continue;
        }
        ESP_LOGD(
            TAG, "ARTEMIS packet received +%lld ms since discovery",
            (long long)((esp_timer_get_time() - discovery_started_us) / 1000));

        // The bulk data channel uses D0/D1 frames on a non-zero subtype
        // (0x03 photo, 0x04 thumbnail); the session's RUDP parser only accepts
        // subtype 0, so handle those here: reassemble the record and ACK the
        // window so the camera advances. These frames are not session DATA, so
        // skip handle_packet() for them.
        if (received >= 8 && packet[0] == 0xf1 && packet[1] == 0xd0 &&
            packet[4] == 0xd1 && packet[5] != 0x00) {
            if (handle_bulk_frame(socket_fd, camera_address, packet,
                                  static_cast<size_t>(received))) {
                last_progress_us_ = last_receive_us;
            }
            continue;
        }

        const SessionEvent event =
            session_.handle_packet(packet, static_cast<size_t>(received));
        if (session_.transport_failed()) {
            return ESP_FAIL;
        }
        if (event.type == SessionEvent::Type::ack && heartbeat_pending &&
            !session_.is_outstanding(heartbeat_index)) {
            ESP_LOGI(TAG, "Heartbeat index %u acknowledged", heartbeat_index);
            heartbeat_pending = false;
            // The session is now settled (a heartbeat round-tripped). Send the
            // first command, as the app does, starting with device info.
            if (logged_in_ && !devinfo_requested_) {
                devinfo_requested_ = true;
                send_artemis_command(512, "", "ARTEMIS GetDevInfo");
            }
        } else {
            if (log_session_event(event, packet, static_cast<size_t>(received),
                                  "in the ARTEMIS session")) {
                return ESP_ERR_INVALID_STATE;
            }
            if (event.type == SessionEvent::Type::data) {
                process_artemis_response(event);
            }
        }
    }
    return ESP_OK;
}

// Runs the ARTEMIS session and removes the partial file of a download that
// the end of the session interrupted.
esp_err_t CameraSession::wait_for_artemis_packets(
    int socket_fd, const struct sockaddr_in *camera_address,
    int64_t discovery_started_us)
{
    const esp_err_t error =
        run_artemis_session(socket_fd, camera_address, discovery_started_us);
    ack_socket_ = -1;
    if (video_.active()) {
        video_.log_transfer();
    }
    // A finished video is kept; only the one being received is dropped.
    video_.complete_pending(true);
    camera::store::sd_writer_flush(true);
    if (media_file_.is_open()) {
        ESP_LOGW(TAG, "Session ended during a download; partial file removed");
        media_file_.abort();
    }
    video_.end_session();
    download_active_ = false;
    return error;
}

// Frees the buffers of the session's containers: clear() and reset() keep
// their capacity, and allocations below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// are in internal RAM. After a session they held about 14 KB of it, so the
// BLE controller of the next search could not start its scan.
void CameraSession::release_session_memory(void)
{
    response_ = camera::artemis::Reassembler();
    bulk_ = camera::artemis::BulkReassembler();
    catalog_ = camera::artemis::MediaCatalog();
    thumbnails_.release();
    deferred_command_.clear();
    std::vector<camera::artemis::CatalogEntry>().swap(download_queue_);
    std::string().swap(camera_id_);
}

// Finds the camera, logs in and runs the session on the camera's UDP socket.
esp_err_t CameraSession::connect_and_run_session(const esp_ip4_addr_t &gateway)
{
    const int64_t discovery_started_us = esp_timer_get_time();
    int socket_fd = -1;
    struct sockaddr_in camera_address = {};
    esp_err_t error = camera::link::discover_camera(
        gateway, discovery_started_us, &socket_fd, &camera_address);
    if (error != ESP_OK || socket_fd < 0) {
        return error;
    }

    struct sockaddr_in data_source = {};
    bool confirmed = false;
    error = camera::link::exchange_prelogin(
        socket_fd, camera_address, transport_, session_, discovery_started_us,
        &data_source, &confirmed);
    if (error == ESP_OK && confirmed) {
        error = camera::link::send_login(socket_fd, data_source, session_,
                                         command_cipher_, settings_.login_user,
                                         settings_.login_password);
        if (error == ESP_OK) {
            error = wait_for_artemis_packets(socket_fd, &data_source,
                                             discovery_started_us);
        }
    }
    close(socket_fd);
    return error;
}

SessionResult CameraSession::run(const esp_ip4_addr_t &gateway,
                                 const SessionSettings &settings)
{
    settings_ = settings;
    camera::link::clear_stop();
    last_camera_id_.clear();
    media_list_error_ = false;
    stored_items_ = 0;
    list_files_ = 0;
    list_failed_ = false;
    work_done_ = false;
    battery_percent_ = -1;
    camera::store::sd_writer_begin_session();
    const esp_err_t error = connect_and_run_session(gateway);
    camera::store::sd_writer_end_session();
    // An action sent and not answered is not repeated: the person asks again.
    action_active_ = false;
    camera::action::service::abandon_running(
        "the camera session ended before the camera answered");
    // A session that ended early (stop, timeout, camera closed) may have
    // stored thumbnails since the last save.
    flush_catalog();
    // All requested work is done: record the sync with the camera.
    if (work_done_ && !camera_id_.empty() &&
        !camera::store::mark_camera_synced(camera_id_, (int64_t)time(NULL))) {
        ESP_LOGW(TAG, "Could not record the sync time for '%s'",
                 camera_id_.c_str());
    }
    SessionResult result;
    result.error = error;
    result.work_completed = work_done_;
    result.media_list_error = media_list_error_;
    result.stored_items = stored_items_;
    result.listed_files = list_files_;
    result.camera_id = last_camera_id_;
    result.battery_percent = battery_percent_;
    release_session_memory();
    return result;
}

}  // namespace camera::link
