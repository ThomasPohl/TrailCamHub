// The camera session: its start and end, and the loop that keeps the ARTEMIS
// session alive and hands what the camera sends to the other parts
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

}  // namespace camera::link
