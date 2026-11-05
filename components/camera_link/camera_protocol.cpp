#include <atomic>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "artemis_crypto.hpp"
#include "artemis_json.hpp"
#include "camera_clock.hpp"
#include "artemis_message.hpp"
#include "artemis_reassembler.hpp"
#include "camera_handshake.hpp"
#include "camera_link_io.hpp"
#include "time_sync.h"
#include "camera_protocol.h"
#include "camera_udp_transport.h"
#include "bulk_reassembler.hpp"
#include "bulk_stream.hpp"
#include "device_info.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "camera_udp_packets.hpp"
#include "mbedtls/base64.h"
#include "media_list.hpp"
#include "media_transfer.hpp"
#include "pppp_session.hpp"
#include "sd_writer.hpp"
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
// Bulk frames (thumbnails, photos, videos) are acknowledged as the app does:
// each frame once, up to CONFIG_CAMERA_BULK_ACK_FRAMES in one ACK, sent when
// it is full or once no frame came for the delay. Acknowledging the last 16
// frames with every ACK (each frame about four times) made the camera send
// many frames again.
#ifdef CONFIG_CAMERA_BULK_ACK_FRAMES
#define CAMERA_BULK_ACK_MAX CONFIG_CAMERA_BULK_ACK_FRAMES
#else
#define CAMERA_BULK_ACK_MAX 17
#endif
#define CAMERA_BULK_ACK_DELAY_US (1 * 1000LL)
// A thumbnail with no new frame for this long is requested again.
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
#ifdef CONFIG_CAMERA_DOWNLOAD_VIDEOS
static const bool kDownloadVideos = true;
#else
static const bool kDownloadVideos = false;
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

static const char *TAG = "trailcamhub_protocol";
using camera::link::from_camera;
using camera::link::log_session_event;
using camera::link::send_udp_request;
using camera::link::set_receive_timeout;
using camera::link::stop_requested;
using SessionEvent = camera::pppp::Session::Event;

static camera::link::UdpTransport s_transport;
// Static because it holds the DATA packets awaiting acknowledgement.
// In PSRAM where the build allows it (about 8 KB; internal RAM is short).
EXT_RAM_BSS_ATTR static camera::pppp::Session s_session(s_transport);

// ARTEMIS command-channel state, reset at the start of each session.
static camera::artemis::Aes128 s_command_cipher =
    camera::artemis::command_cipher();
static camera::artemis::Reassembler s_response;
static bool s_logged_in;
static int64_t s_token;
static bool s_devinfo_requested;
static bool s_media_requested;
static uint32_t s_next_command_seq;
static std::string s_camera_id;
static camera::artemis::MediaCatalog s_catalog;
static std::vector<camera::artemis::CatalogEntry> s_thumb_queue;
static size_t s_thumb_index;
static bool s_thumb_emitted;
static bool s_work_done;
// The session cannot do its work (e.g. the camera has no SD card): close it
// like a finished one, but do not count it as synced.
static bool s_end_session;
// The camera reported an error for its media list (getMediaListRet != 0),
// as it does without an SD card.
static std::atomic<bool> s_media_list_error;
// Thumbnails and files stored on the SD card in the last session.
static std::atomic<uint32_t> s_stored_items;
// Listing only (camera_protocol_set_list_only()): the page being read and the
// files reported so far.
static std::atomic<bool> s_list_only;
static camera_protocol_list_cb_t s_list_callback;
static int64_t s_list_page;
static uint32_t s_list_files;
// Scan: the media-list page being read (merged page by page into s_catalog)
// and the items of the pages read so far.
static int64_t s_scan_page;
static uint32_t s_scan_photos;
static uint32_t s_scan_videos;
static uint32_t s_scan_added;
// Last time the camera answered a request or sent bulk data. Heartbeats do
// not count: the camera answers them even when nothing else happens.
static int64_t s_last_progress_us;
// Battery of the camera of the last session; read by other tasks.
static std::atomic<int> s_last_battery_percent{-1};
static camera::artemis::BulkReassembler s_bulk;
static uint8_t s_bulk_subtype;
// Bulk frames received but not yet acknowledged, all of one subtype.
static uint16_t s_bulk_ack_seqs[CAMERA_BULK_ACK_MAX];
static size_t s_bulk_ack_pending;
static uint8_t s_bulk_ack_subtype;
static bool s_bulk_active;
// Sequence that follows the last completed bulk record, so that a resent
// header frame of that record is not taken for the start of the next one.
static uint16_t s_bulk_next_seq;
static bool s_bulk_next_seq_valid;
// Last frame of the record being received, and how often the current
// thumbnail was requested again after it stalled.
static int64_t s_bulk_last_frame_us;
static unsigned s_thumb_retries;

// Media download state, reset at the start of each session. A download is
// active from its request (cmdId 1285) until its record is complete or it is
// aborted.
static std::vector<camera::artemis::CatalogEntry> s_download_queue;
static size_t s_download_index;
static size_t s_download_stored;
static bool s_download_active;
static int64_t s_download_progress_us;
static int64_t s_download_started_us;
static uint64_t s_download_logged_bytes;
static camera::artemis::BulkStream
    s_download_stream(camera::artemis::kMediaRecordHeaderSize,
                      CAMERA_DOWNLOAD_REORDER_FRAMES);
static camera::store::MediaFileWriter s_media_file;
static camera_protocol_progress_cb_t s_progress_callback;

// Set when writing a video failed: no further downloads.
static bool s_stop_downloads;

static bool send_artemis_command(uint32_t cmd_id, const char *extra_fields,
                                 const char *context);
static void queue_bulk_ack(int socket_fd,
                           const struct sockaddr_in *camera_address,
                           uint8_t subtype, uint16_t seq);
static void send_bulk_ack_before_waiting(size_t size);
static void finish_current_download(bool stored, bool stop_all);
static void report_download_progress();

// Socket and camera of the running session, for sending pending ACKs from
// code that has no access to them.
static int s_ack_socket = -1;
static const struct sockaddr_in *s_ack_camera_address;

// Video download by playback (cmdId 769), on top of s_download_active.
static camera::link::PlaybackDownload s_video(
    s_command_cipher, CAMERA_DOWNLOAD_REORDER_FRAMES,
    {[](uint32_t cmd_id, const char *extra, const char *context) {
         return send_artemis_command(cmd_id, extra, context);
     },
     [](uint16_t seq) {
         queue_bulk_ack(s_ack_socket, s_ack_camera_address, 0x02, seq);
     },
     [](size_t size) { send_bulk_ack_before_waiting(size); },
     [](int64_t now_us) { s_download_progress_us = now_us; },
     [](bool stored, bool stop_all) {
         finish_current_download(stored, stop_all);
     },
     [](const camera::artemis::CatalogEntry &e, bool stored, bool sd_failed) {
         if (stored) {
             s_catalog.set_flag(e.media_id,
                                camera::artemis::MediaCatalog::kFileDownloaded);
             camera::store::save_catalog(s_camera_id, s_catalog);
             ++s_download_stored;
             ++s_stored_items;
         }
         if (sd_failed) {
             s_stop_downloads = true;
         }
         report_download_progress();
     }});

// Sends a token-authenticated ARTEMIS command over the session. `extra_fields`
// is inserted verbatim after cmdId (e.g. ",\"pageNo\":0"); it may be empty.
static bool send_artemis_command(uint32_t cmd_id, const char *extra_fields,
                                 const char *context)
{
    const std::string json = "{\"cmdId\":" + std::to_string(cmd_id) +
                             extra_fields +
                             ",\"token\":" + std::to_string(s_token) + "}";
    const std::vector<uint8_t> packet = camera::artemis::build_request(
        s_command_cipher, s_next_command_seq++, json);
    uint16_t index = 0;
    if (packet.empty() ||
        !s_session.send_data(packet.data(), packet.size(), esp_timer_get_time(),
                             context, &index)) {
        ESP_LOGE(TAG, "Could not send %s", context);
        return false;
    }
    ESP_LOGI(TAG, "Sent %s (cmdId %lu), index %u", context,
             (unsigned long)cmd_id, index);
    return true;
}

static void start_downloads();

// Requests the thumbnail (cmdId 772) for the current queue item, if any. The
// reply streams back on the bulk channel; handle_bulk_frame() stores it and
// advances the queue. Returns false when the queue is exhausted.
static bool request_current_thumbnail()
{
    if (s_thumb_index >= s_thumb_queue.size()) {
        ESP_LOGI(TAG, "Thumbnails complete: %u stored of %u catalog items",
                 (unsigned)s_catalog.count_with_flag(
                     camera::artemis::MediaCatalog::kThumbPresent),
                 (unsigned)s_catalog.size());
        start_downloads();
        return false;
    }
    const camera::artemis::CatalogEntry &e = s_thumb_queue[s_thumb_index];
    const std::string extra =
        ",\"thumbnailReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
        ",\"dirNum\":" + std::to_string(e.dir_num) +
        ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
    return send_artemis_command(772, extra.c_str(), "ARTEMIS GetThumbnail");
}

// Sends one ACK for the bulk frames received since the last one.
static void send_bulk_ack(int socket_fd,
                          const struct sockaddr_in *camera_address)
{
    if (s_bulk_ack_pending == 0) {
        return;
    }
    camera::ArtemisBulkAckRequest ack;
    const bool built =
        ack.build(s_bulk_ack_subtype, s_bulk_ack_seqs, s_bulk_ack_pending);
    s_video.note_ack_sent(s_bulk_ack_subtype, s_bulk_ack_seqs,
                          s_bulk_ack_pending);
    s_bulk_ack_pending = 0;
    if (built && send_udp_request(socket_fd, camera_address, "BULK ACK", ack) !=
                     ESP_OK) {
        s_video.note_ack_failure();
    }
}

// Sends the pending ACK now if writing `size` bytes is about to wait for the
// SD card: the camera would otherwise take the frames received meanwhile for
// lost and send them again.
static void send_bulk_ack_before_waiting(size_t size)
{
    if (s_bulk_ack_pending > 0 && s_ack_socket >= 0 &&
        !camera::store::sd_writer_has_room(size)) {
        send_bulk_ack(s_ack_socket, s_ack_camera_address);
    }
}

// Adds a frame to the next ACK; sends it once it is full.
static void queue_bulk_ack(int socket_fd,
                           const struct sockaddr_in *camera_address,
                           uint8_t subtype, uint16_t seq)
{
    if (s_bulk_ack_pending > 0 && subtype != s_bulk_ack_subtype) {
        send_bulk_ack(socket_fd, camera_address);
    }
    s_bulk_ack_subtype = subtype;
    s_bulk_ack_seqs[s_bulk_ack_pending++] = seq;
    if (s_bulk_ack_pending >= CAMERA_BULK_ACK_MAX) {
        send_bulk_ack(socket_fd, camera_address);
    }
}

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

static void report_download_progress()
{
    if (s_progress_callback != nullptr) {
        s_progress_callback(static_cast<uint32_t>(s_download_index),
                            static_cast<uint32_t>(s_download_stored),
                            static_cast<uint32_t>(s_download_queue.size()));
    }
}

// Requests the current download queue item: a photo as a file (cmdId 1285),
// which streams back on the bulk channel for handle_download_frame(), or a
// video by playback (PlaybackDownload::start()). When the queue is exhausted,
// the session is closed.
static void request_current_download()
{
    // Frames of a stopped video stream may still be on their way.
    if (s_video.draining(esp_timer_get_time())) {
        return;
    }
    s_video.clear_drain();
    while (s_download_index < s_download_queue.size()) {
        const camera::artemis::CatalogEntry &e =
            s_download_queue[s_download_index];
        if (e.file_type != 0) {
            if (s_video.start(e, s_camera_id, s_download_index,
                              s_download_queue.size())) {
                s_download_active = true;
                s_download_started_us = s_video.started_us();
                s_download_progress_us = s_download_started_us;
                return;
            }
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            s_download_index = s_download_queue.size();
            break;
        }
        if (!s_media_file.open(s_camera_id, e.media_id, nullptr,
                               !camera::store::sd_writer_active())) {
            ESP_LOGW(TAG, "Cannot write to the SD card; stopping downloads");
            s_download_index = s_download_queue.size();
            break;
        }
        const std::string extra =
            ",\"downloadReqs\":[{\"fileType\":" + std::to_string(e.file_type) +
            ",\"dirNum\":" + std::to_string(e.dir_num) +
            ",\"mediaNum\":" + std::to_string(e.media_num) + "}]";
        if (send_artemis_command(1285, extra.c_str(), "ARTEMIS Download")) {
            ESP_LOGI(TAG, "Downloading %s dir %lld num %lld (%u of %u)",
                     e.file_type == 0 ? "photo" : "video", (long long)e.dir_num,
                     (long long)e.media_num, (unsigned)(s_download_index + 1),
                     (unsigned)s_download_queue.size());
            s_download_stream.reset();
            s_download_active = true;
            s_download_started_us = esp_timer_get_time();
            s_download_progress_us = s_download_started_us;
            s_download_logged_bytes = 0;
            return;
        }
        s_media_file.abort();
        ++s_download_index;
        report_download_progress();
    }
    s_video.complete_pending(true);
    ESP_LOGI(TAG, "Downloads complete: %u of %u catalog items on the SD card",
             (unsigned)s_catalog.count_with_flag(
                 camera::artemis::MediaCatalog::kFileDownloaded),
             (unsigned)s_catalog.size());
    s_work_done = true;
}

// Builds the download queue once the thumbnails are done. Without enabled
// downloads, the session is closed right away.
static void start_downloads()
{
    s_download_queue =
        s_catalog.missing_downloads(kDownloadPhotos, kDownloadVideos);
    s_download_index = 0;
    s_download_stored = 0;
    s_stop_downloads = false;
    if (kDownloadPhotos || kDownloadVideos) {
        ESP_LOGI(TAG, "%u files to download",
                 (unsigned)s_download_queue.size());
    }
    report_download_progress();
    request_current_download();
}

// Ends the current download: on success the file gets its final name and the
// catalog records it; otherwise the partial file is removed and the item is
// tried again in a later session. Then the next item is requested, unless
// `stop_all` (the SD card failed).
static void finish_current_download(bool stored, bool stop_all)
{
    using camera::artemis::MediaCatalog;
    const camera::artemis::CatalogEntry &e = s_download_queue[s_download_index];
    const bool was_video = s_video.active();
    stop_all = stop_all || s_stop_downloads;
    // A video that ended normally is closed once the SD writer has written
    // it, while the next one is downloaded
    // (PlaybackDownload::complete_pending()).
    const bool deferred = s_video.can_defer(stored);
    // Otherwise wait for the queued writes (or drop them if the file is not
    // kept).
    if (!deferred && !camera::store::sd_writer_flush(!stored) && stored) {
        stored = false;
        stop_all = true;
    }
    uint64_t video_bytes = 0;
    if (was_video) {
        stored = s_video.finish(e, stored, deferred, &video_bytes);
    }
    camera::store::MediaFileWriter &file =
        was_video ? s_video.video_file() : s_media_file;
    if (deferred) {
        // Closed by PlaybackDownload::complete_pending().
    } else if (stored && file.finish(e.media_time)) {
        const int64_t elapsed_ms =
            (esp_timer_get_time() - s_download_started_us) / 1000;
        const uint64_t bytes = was_video
                                   ? video_bytes
                                   : s_download_stream.delivered_bytes() -
                                         s_download_stream.prefix().size();
        ESP_LOGI(TAG, "Stored %s (%llu bytes in %lld ms, %llu KB/s)",
                 file.path().c_str(), (unsigned long long)bytes,
                 (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0 ? bytes / elapsed_ms : 0));
        s_catalog.set_flag(e.media_id, MediaCatalog::kFileDownloaded);
        camera::store::save_catalog(s_camera_id, s_catalog);
        ++s_download_stored;
        ++s_stored_items;
    } else {
        file.abort();
        if (!(was_video && kDiscardVideoData)) {
            ESP_LOGW(TAG, "Download of dir %lld num %lld failed",
                     (long long)e.dir_num, (long long)e.media_num);
        }
    }
    s_download_stream.reset();
    s_download_active = false;
    s_bulk_active = false;
    ++s_download_index;
    if (stop_all) {
        ESP_LOGW(TAG, "SD card write failed; stopping downloads");
        s_download_index = s_download_queue.size();
    }
    report_download_progress();
    request_current_download();
}

// Handles one bulk frame of a running download: passes it to the stream,
// which writes the file in order, and acknowledges the frames it took.
static void handle_download_frame(int socket_fd,
                                  const struct sockaddr_in *camera_address,
                                  uint8_t subtype, uint16_t seq,
                                  const uint8_t *payload, size_t payload_len,
                                  bool is_record_start)
{
    using camera::artemis::BulkStream;
    if (!s_download_stream.header_ready() && !is_record_start) {
        // Leftover of an earlier record: acknowledge it so the camera stops
        // resending it.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return;
    }
    if (!s_download_stream.header_ready()) {
        s_bulk_subtype = subtype;
        s_bulk_active = true;
    }

    bool sink_failed = false;
    const BulkStream::Sink sink = [&sink_failed](const uint8_t *data,
                                                 size_t size) {
        sink_failed = !camera::store::sd_writer_write(s_media_file, data, size);
        return !sink_failed;
    };
    const uint64_t before = s_download_stream.delivered_bytes();
    bool ack = false;
    const BulkStream::Status status =
        s_download_stream.feed(seq, payload, payload_len, sink, ack);
    if (ack) {
        queue_bulk_ack(socket_fd, camera_address, subtype, seq);
    }

    const uint64_t delivered = s_download_stream.delivered_bytes();
    if (delivered > before) {
        s_download_progress_us = esp_timer_get_time();
    }
    if (before == 0 && s_download_stream.header_ready()) {
        ESP_LOGI(TAG, "Download record start: subtype 0x%02x, %u bytes",
                 (unsigned)subtype, (unsigned)s_download_stream.record_len());
    }
    if (delivered - s_download_logged_bytes >=
        CAMERA_DOWNLOAD_LOG_INTERVAL_BYTES) {
        s_download_logged_bytes = delivered;
        ESP_LOGI(TAG, "Download: %llu of %u bytes",
                 (unsigned long long)delivered,
                 (unsigned)s_download_stream.record_len());
    }

    if (status == BulkStream::Status::invalid) {
        if (!sink_failed) {
            const size_t dump = payload_len < 24 ? payload_len : 24;
            ESP_LOGW(TAG, "Download record invalid; first bytes:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        }
        finish_current_download(false, sink_failed);
        return;
    }
    if (status == BulkStream::Status::complete) {
        const auto &prefix = s_download_stream.prefix();
        camera::artemis::MediaRecordHeader header;
        size_t data_offset = 0;
        size_t data_size = 0;
        const camera::artemis::CatalogEntry &e =
            s_download_queue[s_download_index];
        if (camera::artemis::parse_media_record(
                prefix.data(), prefix.size(), header, data_offset, data_size) &&
            (header.dir_num != e.dir_num || header.media_num != e.media_num)) {
            ESP_LOGW(TAG,
                     "Download record is for dir %lld num %lld, requested "
                     "dir %lld num %lld; discarding it",
                     (long long)header.dir_num, (long long)header.media_num,
                     (long long)e.dir_num, (long long)e.media_num);
            finish_current_download(false, false);
            return;
        }
        finish_current_download(true, false);
    }
}

// Aborts the current download if no data arrived for the stall timeout.
// A thumbnail record that stops arriving: the heartbeat is paused during a
// bulk transfer, so without this the camera would end the session. Request
// the thumbnail again, or skip it after a second stall.
static void check_thumbnail_stall(int64_t now_us)
{
    if (!s_bulk_active || s_bulk_subtype != 0x04 ||
        now_us - s_bulk_last_frame_us < CAMERA_THUMBNAIL_STALL_US) {
        return;
    }
    ESP_LOGW(TAG, "Thumbnail stalled at %u of %u frames; %s",
             (unsigned)s_bulk.received_frames(),
             (unsigned)s_bulk.total_frames(),
             s_thumb_retries == 0 ? "requesting it again" : "skipping it");
    s_bulk.reset();
    s_bulk_active = false;
    if (s_thumb_retries == 0) {
        s_thumb_retries = 1;
    } else {
        s_thumb_retries = 0;
        ++s_thumb_index;
    }
    request_current_thumbnail();
}

static void check_download_stall(int64_t now_us)
{
    s_video.complete_pending(false);
    if (s_video.drain_done(now_us) && !s_download_active) {
        request_current_download();
        return;
    }
    if (s_video.receiving()) {
        // The camera does not mark the end of the stream; it stops sending.
        if (s_video.stream_ended(now_us)) {
            ESP_LOGI(TAG, "Video stream ended");
            finish_current_download(true, false);
        }
        return;
    }
    if (s_download_active &&
        now_us - s_download_progress_us >
            CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S * 1000000LL) {
        ESP_LOGW(TAG, "No download data for %d s; skipping the file",
                 CONFIG_CAMERA_DOWNLOAD_STALL_TIMEOUT_S);
        finish_current_download(false, false);
    }
}

// Handles one bulk-stream frame (D0/D1 subtype 0x03/0x04): feeds the
// reassembler, acknowledges the window so the camera advances to the next
// frames, and on completion extracts and emits the JPEG.
static void handle_bulk_frame(int socket_fd,
                              const struct sockaddr_in *camera_address,
                              const uint8_t *packet, size_t size)
{
    using namespace camera::artemis;
    static const uint8_t kSig[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    const uint8_t subtype = packet[5];
    const uint16_t seq = static_cast<uint16_t>((packet[6] << 8) | packet[7]);
    const uint8_t *payload = packet + 8;
    const size_t payload_len = size - 8;
    const bool is_record_start =
        payload_len >= 20 && memcmp(payload, kSig, sizeof(kSig)) == 0;

    // During a video download the stream is on subtype 0x02; the other
    // subtypes are ignored.
    if (s_video.active()) {
        if (subtype == 0x02) {
            s_video.handle_frame(seq, payload, payload_len);
            return;
        }
        // Leftovers of the thumbnails: acknowledge them, so that the camera
        // does not send them again and again, taking airtime from the video.
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        s_video.note_other_subtype_frame(subtype, seq, payload, payload_len);
        return;
    }
    // Leftovers of a stopped video stream.
    if (subtype == 0x02) {
        s_video.note_leftover_frame();
        camera::ArtemisBulkAckRequest stray_ack;
        if (stray_ack.build(subtype, &seq, 1)) {
            send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                             stray_ack);
        }
        return;
    }
    // Thumbnails keep subtype 0x04; any other subtype during a download is
    // the requested file (0x03 for photos).
    if (s_download_active && subtype != 0x04) {
        handle_download_frame(socket_fd, camera_address, subtype, seq, payload,
                              payload_len, is_record_start);
        return;
    }

    if (!s_bulk_active || subtype != s_bulk_subtype) {
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
            return;
        }
        // The camera may resend the header frame of a record that is already
        // complete; a record starting before the end of the last one is such
        // a repeat, not the next record.
        if (s_bulk_next_seq_valid && subtype == s_bulk_subtype &&
            static_cast<int16_t>(seq - s_bulk_next_seq) < 0) {
            camera::ArtemisBulkAckRequest stray_ack;
            if (stray_ack.build(subtype, &seq, 1)) {
                send_udp_request(socket_fd, camera_address, "BULK ACK (stray)",
                                 stray_ack);
            }
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x seq %u: repeated header of a "
                     "finished record ignored (next record from seq %u)",
                     (unsigned)subtype, (unsigned)seq,
                     (unsigned)s_bulk_next_seq);
            return;
        }
        s_bulk.reset();
        s_bulk_subtype = subtype;
        s_bulk_active = true;
    }

    s_bulk_last_frame_us = esp_timer_get_time();
    const bool was_header = s_bulk.header_ready();
    const size_t before = s_bulk.received_frames();
    const BulkReassembler::Status status =
        s_bulk.feed(seq, payload, payload_len);

    // Acknowledge the frame so the camera keeps streaming.
    queue_bulk_ack(socket_fd, camera_address, subtype, seq);

    // Log the record boundary so successive records can be told apart.
    if (!was_header && s_bulk.header_ready()) {
        ESP_LOGI(TAG,
                 "BULK record start: subtype 0x%02x base seq %u, ver %u typ %u "
                 "len %u (%u frames)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)s_bulk.version(),
                 (unsigned)s_bulk.type(), (unsigned)s_bulk.record_len(),
                 (unsigned)s_bulk.total_frames());
    }

    const size_t frames = s_bulk.received_frames();
    if (frames > before && (frames == 1 || frames % 8 == 0)) {
        ESP_LOGI(TAG, "BULK subtype 0x%02x seq %u (%u/%u frames, %u bytes)",
                 (unsigned)subtype, (unsigned)seq, (unsigned)frames,
                 (unsigned)s_bulk.total_frames(),
                 (unsigned)s_bulk.received_bytes());
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
                 BulkReassembler::reason_name(s_bulk.invalid_reason()),
                 (unsigned)payload_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        s_bulk.reset();
        s_bulk_active = false;
        return;
    }
    if (status == BulkReassembler::Status::complete) {
        s_bulk_next_seq =
            static_cast<uint16_t>(s_bulk.base_seq() + s_bulk.total_frames());
        s_bulk_next_seq_valid = true;
        MediaRecordHeader header;
        size_t jpeg_offset = 0;
        size_t jpeg_size = 0;
        const bool parsed =
            parse_media_record(s_bulk.payload(), s_bulk.payload_size(), header,
                               jpeg_offset, jpeg_size);
        if (parsed) {
            ESP_LOGI(TAG,
                     "BULK subtype 0x%02x complete: %u record bytes, "
                     "%u JPEG bytes (dir %lld num %lld)",
                     (unsigned)subtype, (unsigned)s_bulk.received_bytes(),
                     (unsigned)jpeg_size, (long long)header.dir_num,
                     (long long)header.media_num);
        }

        if (parsed && subtype == 0x04) {
            // Thumbnail: store it on the SD card and record it in the catalog.
            // Date the file by the capture time from the media list.
            const uint8_t *jpeg = s_bulk.payload() + jpeg_offset;
            // The answer names dir/num, which is unique among the items of
            // the current media list (only those are requested).
            const CatalogEntry *entry =
                s_catalog.find_listed(header.dir_num, header.media_num);
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
                if (camera::store::write_thumbnail(s_camera_id, media_id, jpeg,
                                                   jpeg_size,
                                                   entry->media_time)) {
                    s_catalog.set_flag(media_id, MediaCatalog::kThumbPresent);
                    camera::store::save_catalog(s_camera_id, s_catalog);
                    ++s_stored_items;
                } else {
                    ESP_LOGW(TAG, "Could not store thumbnail dir %lld num %lld",
                             (long long)header.dir_num,
                             (long long)header.media_num);
                }
            }
            // Emit the first thumbnail of the cycle over serial (for the web
            // live view); the rest just go to the SD card to avoid flooding.
            if (CAMERA_THUMBNAIL_SERIAL_PREVIEW && !s_thumb_emitted) {
                emit_thumbnail_jpeg(jpeg, jpeg_size, header);
                s_thumb_emitted = true;
            }
        }

        s_bulk.reset();
        s_bulk_active = false;

        if (subtype == 0x04) {
            ++s_thumb_index;
            s_thumb_retries = 0;
            request_current_thumbnail();
        }
    }
}

// --- Listing only ---

static void report_list_line(const std::string &line)
{
    if (s_list_callback != nullptr) {
        s_list_callback(line.c_str());
    }
}

// Ends a listing with its final line.
static void finish_list(const std::string &line)
{
    report_list_line(line);
    s_end_session = true;
}

static void request_list_page()
{
    const std::string fields =
        camera::artemis::media_list_request_fields(s_list_page);
    if (!send_artemis_command(768, fields.c_str(), "ARTEMIS GetMediaList")) {
        finish_list("list failed could not send the request");
    }
}

static void handle_list_page(const std::string &json)
{
    camera::artemis::MediaList list;
    const bool parsed = camera::artemis::parse_media_list(json, list);
    if (!parsed || camera::artemis::media_list_failed(list)) {
        finish_list("list failed getMediaListRet=" +
                    std::to_string(list.list_ret));
        return;
    }
    if (list.page_no != s_list_page) {
        finish_list("list failed asked for page " +
                    std::to_string(s_list_page) + ", got " +
                    std::to_string(list.page_no));
        return;
    }
    for (const camera::artemis::MediaItem &item : list.files) {
        report_list_line("media " + camera::artemis::format_media_item(item));
        ++s_list_files;
    }
    ++s_list_page;
    const std::string counts = " files=" + std::to_string(s_list_files) +
                               " pages=" + std::to_string(s_list_page);
    if (!camera::artemis::media_list_has_more(list)) {
        finish_list("list done" + counts);
    } else if (s_list_page >= camera::artemis::kMediaListMaxPages) {
        finish_list("list truncated" + counts);
    } else {
        request_list_page();
    }
}

// --- Media list of a scan ---

static void request_scan_page()
{
    const std::string fields =
        camera::artemis::media_list_request_fields(s_scan_page);
    if (!send_artemis_command(768, fields.c_str(), "ARTEMIS GetMediaList")) {
        ESP_LOGE(TAG, "Could not request media-list page %lld",
                 (long long)s_scan_page);
        s_end_session = true;
    }
}

// All pages are merged: persist the catalog, then fetch the thumbnail of
// every listed item we do not have one for yet (newest first).
static void finish_scan_list()
{
    // The web protocol view reads the "Media list parsed: N items" prefix.
    ESP_LOGI(TAG,
             "Media list parsed: %u items (%u photos, %u videos), %lld "
             "page(s)",
             (unsigned)(s_scan_photos + s_scan_videos), (unsigned)s_scan_photos,
             (unsigned)s_scan_videos, (long long)s_scan_page);
    camera::store::save_catalog(s_camera_id, s_catalog);
    s_thumb_queue = s_catalog.missing_thumbnails();
    s_thumb_index = 0;
    s_thumb_emitted = false;
    ESP_LOGI(TAG, "Catalog %s: %u items (%u new), %u thumbnails to fetch",
             s_camera_id.c_str(), (unsigned)s_catalog.size(),
             (unsigned)s_scan_added, (unsigned)s_thumb_queue.size());
    request_current_thumbnail();
}

// One page of the media list. The camera does not report a total, so the
// list ends with a short (or empty) page; each page is merged on its own, so
// only one page is held in RAM.
static void handle_scan_page(const std::string &json)
{
    camera::artemis::MediaList list;
    const bool parsed = camera::artemis::parse_media_list(json, list);
    if (!parsed || camera::artemis::media_list_failed(list)) {
        ESP_LOGE(TAG,
                 "Media list page %lld failed (getMediaListRet %lld, "
                 "errorMsg '%s'%s)",
                 (long long)s_scan_page, (long long)list.list_ret,
                 list.error_msg.c_str(), parsed ? "" : ", no mediaFiles array");
        if (s_scan_page == 0) {
            // Nothing to fetch: no thumbnail or download would be queued, so
            // the session would idle on heartbeats. Leave the catalog as is.
            ESP_LOGE(TAG, "Ending the camera session");
            s_media_list_error =
                parsed && camera::artemis::media_list_failed(list);
            s_end_session = true;
        } else {
            // Work with the pages read so far.
            finish_scan_list();
        }
        return;
    }
    if (list.page_no != s_scan_page) {
        // A late answer to an earlier request; the answer to this one follows.
        ESP_LOGW(TAG, "Ignoring media-list page %lld (expected %lld)",
                 (long long)list.page_no, (long long)s_scan_page);
        return;
    }

    for (const auto &item : list.files) {
        if (item.file_type == 0)
            ++s_scan_photos;
        else
            ++s_scan_videos;
    }
    const size_t added = s_catalog.merge_page(list);
    s_scan_added += (uint32_t)added;
    ESP_LOGI(TAG, "Media list page %lld: %u items (%u new), cnt %lld",
             (long long)s_scan_page, (unsigned)list.files.size(),
             (unsigned)added, (long long)list.count);
    ++s_scan_page;
    if (!camera::artemis::media_list_has_more(list)) {
        finish_scan_list();
    } else if (s_scan_page >= CONFIG_CAMERA_MEDIA_LIST_MAX_PAGES) {
        ESP_LOGW(TAG,
                 "Media list has more than %d pages; older files are not "
                 "read in this session",
                 CONFIG_CAMERA_MEDIA_LIST_MAX_PAGES);
        finish_scan_list();
    } else {
        request_scan_page();
    }
}

// Decrypts a received ARTEMIS DATA message (reassembling fragments). On the
// login response it records the session token; the first command is sent only
// once the session has settled (see the heartbeat-ack path), mirroring the
// app's order. Other responses are logged as decrypted JSON, and the
// device-info reply triggers the media-list request.
static void process_artemis_response(const SessionEvent &event)
{
    using namespace camera::artemis;

    const Reassembler::Status status =
        s_response.feed(event.payload, event.payload_size);
    if (status == Reassembler::Status::incomplete) {
        return;
    }
    if (status == Reassembler::Status::invalid) {
        s_response.reset();
        return;
    }

    Message message;
    const bool decoded = parse(s_command_cipher, s_response.message().data(),
                               s_response.message().size(), message);
    s_response.reset();
    if (!decoded) {
        ESP_LOGW(TAG, "Could not decode the ARTEMIS response");
        return;
    }

    int64_t cmd_id = -1;
    json_find_int(message.json, "cmdId", cmd_id);

    if (cmd_id == 0) {
        if (s_logged_in) {
            return;
        }
        int64_t result = -1;
        int64_t token = 0;
        if (json_find_int(message.json, "result", result) && result == 0 &&
            json_find_int(message.json, "token", token)) {
            s_token = token;
            s_logged_in = true;
            ESP_LOGI(TAG, "ARTEMIS login succeeded; token %lld",
                     (long long)token);
        } else {
            ESP_LOGW(TAG, "ARTEMIS login rejected: %s", message.json.c_str());
        }
        return;
    }

    ESP_LOGI(TAG, "ARTEMIS response cmdId %lld: %s", (long long)cmd_id,
             message.json.c_str());
    if (cmd_id != 525) {
        s_last_progress_us = esp_timer_get_time();
    }

    // The camera answers the playback request with the video's size, then
    // streams it. When it refuses the app's fileType, try the media list's.
    if (cmd_id == 769 && s_video.active()) {
        if (s_video.handle_playback_response(
                message.json, s_download_queue[s_download_index]) ==
            camera::link::PlaybackDownload::Response::refused) {
            finish_current_download(false, false);
        }
        return;
    }

    // The camera acknowledges a download request before streaming the file;
    // a non-zero result means it will not send it.
    if (cmd_id == 1285 && s_download_active) {
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
    if (cmd_id == 768 && s_list_only.load()) {
        handle_list_page(message.json);
        return;
    }
    if (cmd_id == 768) {
        handle_scan_page(message.json);
    }

    // Device info carries the camera identity (devId) and battery state. Use
    // the id to load that camera's catalog from the SD card, then request the
    // media list (as the app does; 768 needs paging parameters to answer).
    if (cmd_id == 512 && s_logged_in && !s_media_requested) {
        DeviceInfo info;
        if (parse_device_info(message.json, info)) {
            s_camera_id = info.dev_id;
            if (info.bat_percent >= 0 && info.bat_percent <= 100) {
                s_last_battery_percent = (int)info.bat_percent;
            }
            ESP_LOGI(TAG,
                     "Camera '%s' model '%s' fw '%s' battery %lld%% "
                     "SD free %lld MB",
                     s_camera_id.c_str(), info.model_name.c_str(),
                     info.fw_version.c_str(), (long long)info.bat_percent,
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
            s_camera_id = "unknown";
            ESP_LOGW(TAG, "Could not parse device info; using camera id '%s'",
                     s_camera_id.c_str());
        }
        s_media_requested = true;
        if (s_list_only.load()) {
            s_list_page = 0;
            s_list_files = 0;
            request_list_page();
            return;
        }
        s_catalog.clear();
        camera::store::load_catalog(s_camera_id, s_catalog);
        ESP_LOGI(TAG, "Loaded catalog: %u known items",
                 (unsigned)s_catalog.size());

        s_catalog.begin_listing();
        s_scan_page = 0;
        s_scan_photos = 0;
        s_scan_videos = 0;
        s_scan_added = 0;
        request_scan_page();
    }
}

// Keeps the ARTEMIS session alive with heartbeats (one at a time, sent
// after the previous one was acknowledged) and logs everything received
// until the camera closes the session or a stop is requested.
static esp_err_t run_artemis_session(int socket_fd,
                                     const struct sockaddr_in *camera_address,
                                     int64_t discovery_started_us)
{
    camera::ArtemisHeartbeatPayload heartbeat;
    uint16_t heartbeat_index = 0;
    bool heartbeat_pending = false;
    s_ack_socket = socket_fd;
    s_ack_camera_address = camera_address;
    int64_t last_heartbeat_us = 0;
    int64_t heartbeat_interval_us = CAMERA_RUDP_HEARTBEAT_INTERVAL_MS * 1000LL;
    int64_t last_receive_us = 0;
    s_response.reset();
    s_logged_in = false;
    s_token = 0;
    s_devinfo_requested = false;
    s_media_requested = false;
    s_next_command_seq = 2;
    s_camera_id.clear();
    s_catalog.clear();
    s_thumb_queue.clear();
    s_thumb_index = 0;
    s_thumb_emitted = false;
    s_scan_page = 0;
    s_work_done = false;
    s_end_session = false;
    s_last_progress_us = esp_timer_get_time();
    s_bulk.reset();
    s_bulk_subtype = 0;
    s_bulk_ack_pending = 0;
    s_bulk_active = false;
    s_bulk_next_seq_valid = false;
    s_thumb_retries = 0;
    s_download_queue.clear();
    s_download_index = 0;
    s_download_active = false;
    s_download_stream.reset();
    s_video.begin_session();
    ESP_LOGI(TAG, "Starting ARTEMIS heartbeat loop");

    while (!stop_requested()) {
        // All requested work is done: close the camera session cleanly
        // (PPPP/CS2 CLOSE) and finish, rather than idling on heartbeats.
        if (s_work_done || s_end_session) {
            const camera::ControlRequest close(camera::PacketType::close);
            send_udp_request(socket_fd, camera_address, "CLOSE", close);
            ESP_LOGI(TAG, "%s; closed the camera session",
                     s_work_done ? "Work complete" : "Nothing more to do");
            // A listing ends this way when it is done. A scan that could not
            // do its work (e.g. no SD card in the camera) did not read the
            // camera.
            return s_work_done || s_list_only.load() ? ESP_OK
                                                     : ESP_ERR_INVALID_RESPONSE;
        }
        int64_t now_us = esp_timer_get_time();
        // A request the camera never answers (or an answer we do not
        // handle) would keep the session, and the camera's Wi-Fi, alive on
        // heartbeats until its battery is empty.
        if (now_us - s_last_progress_us >
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
        // A video download may use its own interval: the camera streamed about
        // two video frames per heartbeat, as if it were held back by them.
        heartbeat_interval_us =
            (s_video.active() ? CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS
                              : CAMERA_RUDP_HEARTBEAT_INTERVAL_MS) *
            1000LL;
        // Pause the session heartbeat during a bulk transfer: it would not be
        // acknowledged while the camera is streaming, and its retransmissions
        // only compete with our frame ACKs on the uplink.
        if (!s_bulk_active && !heartbeat_pending &&
            (last_heartbeat_us == 0 ||
             now_us - last_heartbeat_us >= heartbeat_interval_us)) {
            const uint8_t *body = heartbeat.next_command();
            if (s_session.send_data(body, heartbeat.size(), now_us,
                                    "ARTEMIS heartbeat", &heartbeat_index)) {
                heartbeat_pending = true;
                last_heartbeat_us = now_us;
                s_video.note_heartbeat();
                ESP_LOGI(TAG,
                         "ARTEMIS heartbeat command %u, index %u awaiting ACK",
                         heartbeat.command_count(), heartbeat_index);
            } else if (!s_session.transport_failed()) {
                if (s_session.outstanding_count() <
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
        size_t retransmitted = s_bulk_active ? 0 : s_session.poll(now_us);
        if (retransmitted > 0) {
            ESP_LOGW(TAG, "Retransmitted %u unacknowledged DATA packet(s)",
                     (unsigned)retransmitted);
        }
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }

        // Wake up in time for the next retransmission or heartbeat. During a
        // bulk transfer both are paused, so just block for the full interval
        // and stay responsive to incoming frames.
        int64_t timeout_us = CAMERA_RUDP_RECEIVE_TIMEOUT_MS * 1000LL;
        if (!s_bulk_active) {
            timeout_us = s_session.time_until_retransmit_us(now_us, timeout_us);
            if (!heartbeat_pending) {
                int64_t heartbeat_due_us =
                    heartbeat_interval_us - (now_us - last_heartbeat_us);
                if (heartbeat_due_us < timeout_us) {
                    timeout_us = heartbeat_due_us;
                }
            }
        }
        // Send the pending bulk ACK once no frame follows shortly.
        if (s_bulk_ack_pending > 0 && timeout_us > CAMERA_BULK_ACK_DELAY_US) {
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
        if (s_video.active() && last_receive_us != 0) {
            s_video.note_loop_busy(esp_timer_get_time() - last_receive_us);
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
            s_last_progress_us = last_receive_us;
            handle_bulk_frame(socket_fd, camera_address, packet,
                              static_cast<size_t>(received));
            continue;
        }

        const SessionEvent event =
            s_session.handle_packet(packet, static_cast<size_t>(received));
        if (s_session.transport_failed()) {
            return ESP_FAIL;
        }
        if (event.type == SessionEvent::Type::ack && heartbeat_pending &&
            !s_session.is_outstanding(heartbeat_index)) {
            ESP_LOGI(TAG, "Heartbeat index %u acknowledged", heartbeat_index);
            heartbeat_pending = false;
            // The session is now settled (a heartbeat round-tripped). Send the
            // first command, as the app does, starting with device info.
            if (s_logged_in && !s_devinfo_requested) {
                s_devinfo_requested = true;
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
static esp_err_t
wait_for_artemis_packets(int socket_fd,
                         const struct sockaddr_in *camera_address,
                         int64_t discovery_started_us)
{
    const esp_err_t error =
        run_artemis_session(socket_fd, camera_address, discovery_started_us);
    s_ack_socket = -1;
    if (s_video.active()) {
        s_video.log_transfer();
    }
    // A finished video is kept; only the one being received is dropped.
    s_video.complete_pending(true);
    camera::store::sd_writer_flush(true);
    if (s_media_file.is_open()) {
        ESP_LOGW(TAG, "Session ended during a download; partial file removed");
        s_media_file.abort();
    }
    s_video.end_session();
    s_download_active = false;
    return error;
}

// Frees the buffers of the session's containers: clear() and reset() keep
// their capacity, and allocations below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// are in internal RAM. After a session they held about 14 KB of it, so the
// BLE controller of the next search could not start its scan.
static void release_session_memory(void)
{
    s_response = camera::artemis::Reassembler();
    s_bulk = camera::artemis::BulkReassembler();
    s_catalog = camera::artemis::MediaCatalog();
    std::vector<camera::artemis::CatalogEntry>().swap(s_thumb_queue);
    std::vector<camera::artemis::CatalogEntry>().swap(s_download_queue);
    std::string().swap(s_camera_id);
}

// Finds the camera, logs in and runs the session on the camera's UDP socket.
static esp_err_t connect_and_run_session(const esp_ip4_addr_t &gateway)
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
        socket_fd, camera_address, s_transport, s_session, discovery_started_us,
        &data_source, &confirmed);
    if (error == ESP_OK && confirmed) {
        error = camera::link::send_login(socket_fd, data_source, s_session,
                                         s_command_cipher);
        if (error == ESP_OK) {
            error = wait_for_artemis_packets(socket_fd, &data_source,
                                             discovery_started_us);
        }
    }
    close(socket_fd);
    return error;
}

esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway)
{
    if (gateway == NULL || gateway->addr == 0) {
        ESP_LOGE(TAG, "Cannot start camera protocol without a valid gateway");
        return ESP_ERR_INVALID_ARG;
    }
    camera::link::clear_stop();
    s_media_list_error = false;
    s_stored_items = 0;
    s_work_done = false;
    camera::store::sd_writer_begin_session();
    const esp_err_t error = connect_and_run_session(*gateway);
    camera::store::sd_writer_end_session();
    // All requested work is done: record the sync with the camera.
    if (s_work_done && !s_camera_id.empty() &&
        !camera::store::mark_camera_synced(s_camera_id, (int64_t)time(NULL))) {
        ESP_LOGW(TAG, "Could not record the sync time for '%s'",
                 s_camera_id.c_str());
    }
    release_session_memory();
    return error;
}

void camera_protocol_request_stop(void)
{
    camera::link::request_stop();
}

bool camera_protocol_work_completed(void)
{
    return s_work_done;
}

bool camera_protocol_media_list_error(void)
{
    return s_media_list_error;
}

uint32_t camera_protocol_stored_items(void)
{
    return s_stored_items;
}

int camera_protocol_last_battery_percent(void)
{
    return s_last_battery_percent.load();
}

void camera_protocol_init_storage(void)
{
    camera::store::sd_writer_start();
}

void camera_protocol_set_list_only(bool list_only)
{
    s_list_only = list_only;
}

void camera_protocol_set_list_callback(camera_protocol_list_cb_t cb)
{
    s_list_callback = cb;
}

void camera_protocol_set_progress_callback(camera_protocol_progress_cb_t cb)
{
    s_progress_callback = cb;
}
