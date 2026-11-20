#pragma once

// One camera session of the hub (#65): finding the camera, logging in, and
// then the ARTEMIS session, in which the device info, the media list, the
// thumbnails and the files are fetched and the queued actions are run, until
// the work is done, the camera closes the session, a stop is requested or the
// camera stops answering. ESP-IDF only.
//
// All the state of a session is in this object and not in the file statics of
// the source file. It is meant to be used for one session at a time (a single
// task), and long-lived: its buffers are big and in internal RAM, so it is a
// static object, not one made for each session. run() starts from the same
// state every time (see begin_session()).
//
// camera_protocol.cpp is the C interface to it, with the settings that other
// tasks make before a session and the results they read after it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "artemis_crypto.hpp"
#include "artemis_login.hpp"
#include "artemis_reassembler.hpp"
#include "bulk_reassembler.hpp"
#include "bulk_side_channel.hpp"
#include "bulk_stream.hpp"
#include "camera_action.hpp"
#include "camera_link_io.hpp"
#include "catalog_file.hpp"
#include "deferred_command.hpp"
#include "device_info.hpp"
#include "media_catalog.hpp"
#include "media_list_flow.hpp"
#include "media_store.hpp"
#include "playback_download.hpp"
#include "pppp_session.hpp"
#include "thumbnail_queue.hpp"

#include "sdkconfig.h"

namespace camera::link
{

// Bulk frames (thumbnails, photos, videos) are acknowledged as the app does:
// each frame once, up to CONFIG_CAMERA_BULK_ACK_FRAMES in one ACK, sent when
// it is full or once no frame came for a short delay. Acknowledging the last
// 16 frames with every ACK (each frame about four times) made the camera send
// many frames again.
#ifdef CONFIG_CAMERA_BULK_ACK_FRAMES
constexpr std::size_t kBulkAckMax = CONFIG_CAMERA_BULK_ACK_FRAMES;
#else
constexpr std::size_t kBulkAckMax = 17;
#endif

// What the other tasks set before a session (the camera_protocol_set_*
// functions). A session works with a copy taken when it starts.
struct SessionSettings {
    // Only list the camera's media, to `list_callback` (one line each, then a
    // final line); no thumbnail or file is fetched and the catalog is not
    // touched.
    bool list_only = false;
    // A short session for the queued actions: device info, the actions (and,
    // for a deletion, the media list), then CLOSE.
    bool actions_only = false;
    // In a build that downloads only requested videos: fetch every missing
    // video.
    bool all_videos = false;
    // The camera login; set from the settings, never empty.
    std::string login_user = camera::artemis::kDefaultUser;
    std::string login_password = camera::artemis::kDefaultPassword;
    // Lines of a listing (see camera_protocol_set_list_callback()).
    void (*list_callback)(const char *line) = nullptr;
    // Progress of the downloads (see camera_protocol_set_progress_callback()).
    void (*progress_callback)(std::uint32_t done, std::uint32_t stored,
                              std::uint32_t total) = nullptr;
};

// How a session ended and what it did.
struct SessionResult {
    // ESP_OK: all work done, a listing done or a stop requested;
    // ESP_ERR_INVALID_STATE: the camera closed the session;
    // ESP_ERR_INVALID_RESPONSE: the work could not be done (e.g. no SD card
    // in the camera) or the listing failed; ESP_ERR_TIMEOUT: the camera made
    // no progress; another error: the transport failed.
    esp_err_t error = ESP_OK;
    // All requested work was done (not a stop, not an error).
    bool work_completed = false;
    // The camera reported an error for its media list (getMediaListRet != 0).
    bool media_list_error = false;
    // Thumbnails and files stored on the SD card, also when it ended with an
    // error.
    std::uint32_t stored_items = 0;
    // Files reported by a listing.
    std::uint32_t listed_files = 0;
    // The camera's devId as for its directory on the SD card and the backend
    // (at most 47 characters); empty if it did not report its device info.
    std::string camera_id;
    // Battery of the camera in percent from its device info; -1 if it did not
    // report one.
    int battery_percent = -1;
};

class CameraSession
{
  public:
    // `transport` sends the session's packets and `session` is the reliable
    // UDP layer on top of it. `session` is static, in PSRAM where the build
    // allows it (it holds the DATA packets awaiting acknowledgement).
    CameraSession(UdpTransport &transport, camera::pppp::Session &session);

    CameraSession(const CameraSession &) = delete;
    CameraSession &operator=(const CameraSession &) = delete;

    // Finds the camera at `gateway`, logs in and runs the session. Blocks
    // until it ends. Safe to run again afterwards.
    SessionResult run(const esp_ip4_addr_t &gateway,
                      const SessionSettings &settings);

  private:
    using SessionEvent = camera::pppp::Session::Event;

    // --- start, end and the loop of the session ---
    esp_err_t connect_and_run_session(const esp_ip4_addr_t &gateway);
    esp_err_t wait_for_artemis_packets(int socket_fd,
                                       const struct sockaddr_in *camera_address,
                                       int64_t discovery_started_us);
    esp_err_t run_artemis_session(int socket_fd,
                                  const struct sockaddr_in *camera_address,
                                  int64_t discovery_started_us);
    void release_session_memory();
    camera::artemis::MediaCatalog::VideoDownload video_download() const;

    // --- command channel ---
    bool send_artemis_command(uint32_t cmd_id, const char *extra_fields,
                              const char *context);
    void send_deferred_command(int64_t now_us);
    void process_artemis_response(const SessionEvent &event);

    // --- catalog on the SD card ---
    void save_session_catalog();
    void flush_catalog();

    // --- bulk channel: thumbnails and acknowledgements ---
    bool request_current_thumbnail();
    void begin_thumbnails();
    void send_bulk_ack(int socket_fd, const struct sockaddr_in *camera_address);
    void send_bulk_ack_before_waiting(size_t size);
    void queue_bulk_ack(int socket_fd, const struct sockaddr_in *camera_address,
                        uint8_t subtype, uint16_t seq);
    void check_thumbnail_stall(int64_t now_us);
    bool handle_bulk_frame(int socket_fd,
                           const struct sockaddr_in *camera_address,
                           const uint8_t *packet, size_t size);

    // --- downloads ---
    void report_download_progress();
    void start_downloads();
    void request_current_download();
    void finish_current_download(bool stored, bool stop_all);
    bool handle_download_frame(int socket_fd,
                               const struct sockaddr_in *camera_address,
                               uint8_t subtype, uint16_t seq,
                               const uint8_t *payload, size_t payload_len,
                               bool is_record_start);
    void check_download_stall(int64_t now_us);

    // --- media list of a listing ---
    void report_list_line(const std::string &line);
    void finish_list(const std::string &line);
    void record_sd_card(const camera::artemis::MediaList &list,
                        camera::link::SdReport report);
    void fail_list(const std::string &reason);
    void request_list_page();
    void handle_list_page(const std::string &json);

    // --- media list of a scan ---
    void request_scan_page();
    void finish_scan_list(bool complete);
    void handle_scan_page(const std::string &json);

    // --- queued actions (#194) ---
    void begin_listing_phase();
    void after_list_actions();
    bool send_next_action();
    void advance_after_actions();
    void handle_action_response(const std::string &json);
    void check_action_timeout(int64_t now_us);

    // The settings of the running session.
    SessionSettings settings_;

    UdpTransport &transport_;
    camera::pppp::Session &session_;

    // ARTEMIS command-channel state, reset at the start of each session.
    camera::artemis::Aes128 command_cipher_;
    camera::artemis::Reassembler response_;
    bool logged_in_ = false;
    int64_t token_ = 0;
    bool devinfo_requested_ = false;
    bool media_requested_ = false;
    uint32_t next_command_seq_ = 0;
    std::string camera_id_;
    // Sanitized id of the camera of the session (see
    // SessionResult::camera_id).
    std::string last_camera_id_;
    camera::artemis::MediaCatalog catalog_;
    // Storing a thumbnail does not rewrite the catalog each time: it is saved
    // after this many thumbnails or seconds, when the thumbnails are complete
    // and when the session ends. Thumbnails stored after the last save are
    // fetched again if the power fails.
    camera::store::SaveThrottle catalog_saves_;
    camera::link::ThumbnailQueue thumbnails_;
    // When the thumbnail the session waits for was requested; 0 when none is
    // awaited (before the thumbnails and after the last one).
    int64_t thumbnail_requested_us_ = 0;
    // A command that found every DATA slot of the session in use.
    camera::link::DeferredCommand deferred_command_;
    bool thumb_emitted_ = false;
    bool work_done_ = false;
    // The session cannot do its work (e.g. the camera has no SD card): close
    // it like a finished one, but do not count it as synced.
    bool end_session_ = false;
    // The camera reported an error for its media list (getMediaListRet != 0),
    // as it does without an SD card.
    bool media_list_error_ = false;
    // Thumbnails and files stored on the SD card in the session.
    uint32_t stored_items_ = 0;
    // The camera's device info of this session, for checking an action.
    camera::artemis::DeviceInfo device_info_;
    // The action whose answer the session waits for (see send_next_action()).
    bool action_active_ = false;
    uint32_t action_id_ = 0;
    camera::action::Action action_;
    int64_t action_sent_us_ = 0;
    // The media list of this session is complete (or as far as it was read),
    // so a deletion may look its file up.
    bool list_ready_ = false;
    // Listing only: the page being read and the files reported so far.
    camera::link::ListingFlow listing_;
    uint32_t list_files_ = 0;
    // The listing ended with "list failed ...".
    bool list_failed_ = false;
    // Scan: the media list, merged page by page into the catalog.
    camera::link::ScanListFlow scan_list_;
    // Last time the camera answered a request or sent bulk data. Heartbeats do
    // not count: the camera answers them even when nothing else happens.
    int64_t last_progress_us_ = 0;
    // Battery of the camera of the session.
    int battery_percent_ = -1;
    camera::artemis::BulkReassembler bulk_;
    uint8_t bulk_subtype_ = 0;
    // Bulk frames received but not yet acknowledged, all of one subtype.
    uint16_t bulk_ack_seqs_[kBulkAckMax] = {};
    size_t bulk_ack_pending_ = 0;
    uint8_t bulk_ack_subtype_ = 0;
    bool bulk_active_ = false;
    // Sequence that follows the last completed bulk record, so that a resent
    // header frame of that record is not taken for the start of the next one.
    uint16_t bulk_next_seq_ = 0;
    bool bulk_next_seq_valid_ = false;
    // Last frame of the record being received.
    int64_t bulk_last_frame_us_ = 0;
    // Bulk frames outside the requested media (see bulk_side_channel.hpp).
    camera::artemis::BulkSideChannel side_channel_;

    // Media download state, reset at the start of each session. A download is
    // active from its request (cmdId 1285) until its record is complete or it
    // is aborted.
    std::vector<camera::artemis::CatalogEntry> download_queue_;
    size_t download_index_ = 0;
    size_t download_stored_ = 0;
    bool download_active_ = false;
    int64_t download_progress_us_ = 0;
    int64_t download_started_us_ = 0;
    uint64_t download_logged_bytes_ = 0;
    camera::artemis::BulkStream download_stream_;
    camera::store::MediaFileWriter media_file_;
    // Set when writing a video failed: no further downloads.
    bool stop_downloads_ = false;

    // Socket and camera of the running session, for sending pending ACKs from
    // code that has no access to them.
    int ack_socket_ = -1;
    const struct sockaddr_in *ack_camera_address_ = nullptr;

    // Video download by playback (cmdId 769), on top of download_active_.
    camera::link::PlaybackDownload video_;
};

}  // namespace camera::link
