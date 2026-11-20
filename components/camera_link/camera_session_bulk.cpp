// The camera session's bulk channel: thumbnails and the acknowledgement of
// bulk frames (camera_session.hpp).

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

// Requests the thumbnail (cmdId 772) for the current queue item, if any.
// The reply streams back on the bulk channel; handle_bulk_frame() stores it
// and advances the queue. Returns false when the queue is exhausted or the
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

// Sends the pending ACK now if writing `size` bytes is about to wait for
// the SD card: the camera would otherwise take the frames received
// meanwhile for lost and send them again.
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

// A thumbnail record that stops arriving: the heartbeat is paused during a
// bulk transfer, so without this the camera would end the session. Request
// the thumbnail again, or skip it after a second stall. The same goes for a
// request that brings no record at all (it could not be sent, the camera
// lost it, or the record was rejected): nothing else would ask again, and
// the session would idle on heartbeats until its watchdog.
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

// Handles one bulk-stream frame (D0/D1 subtype 0x03/0x04): feeds the
// reassembler, acknowledges the window so the camera advances to the next
// frames, and on completion extracts and emits the JPEG. Returns whether
// the frame advanced the work: a new frame of the current record, download
// or video. Stray frames between records and repeats do not count, so a
// camera that only resends them does not keep the session alive (watchdog).
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
        // does not send them again and again, taking airtime from the
        // video.
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
    // Records the camera sends on its own: acknowledge them, so that it
    // does not send them again and again, and leave the record being
    // received alone. They are no progress of the session.
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
        // other frame received between records is a retransmit of a
        // finished record or a reordered straggler: acknowledge it so the
        // camera stops resending and moves on, but do not start a record on
        // it.
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
        // The camera may resend the header frame of a record that is
        // already complete; a record starting before the end of the last
        // one is such a repeat, not the next record.
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
        // bytes, so a non-header frame arriving as a record start
        // (reordering, a straggler, or an unexpected framing) is visible in
        // the log.
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
            // The thumbnail is skipped; the stall handling asks for it
            // again or moves on.
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
            // Thumbnail: store it on the SD card and record it in the
            // catalog. Date the file by the capture time from the media
            // list.
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
            // Emit the first thumbnail of the cycle over serial (for the
            // web live view); the rest just go to the SD card to avoid
            // flooding.
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

}  // namespace camera::link
