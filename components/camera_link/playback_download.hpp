#pragma once

// Download of a video by playback (cmdId 769): the camera plays the video
// back and streams its H.264 and AAC frames on bulk subtype 0x02, which are
// decoded while they arrive and written to <mediaId>.h264 and <mediaId>.aac.
// The download queue, the photo download and the bulk channel stay in
// camera_protocol.cpp, which drives this class through the calls below and
// gets its requests back through Host. ESP-IDF only; the decisions are in
// playback_policy.hpp and the figures in video_stats.hpp.

#include <cstddef>
#include <cstdint>
#include <string>

#include "artemis_aes128.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "playback_decoder.hpp"
#include "playback_policy.hpp"
#include "playback_stream.hpp"
#include "video_stats.hpp"
#include "sdkconfig.h"

namespace camera::link
{

class PlaybackDownload
{
  public:
    // What the download asks of the session. Plain function pointers, which
    // get `context` as their first argument (the session): std::function
    // would cost internal RAM on boards that are short of it.
    struct Host {
        void *context;
        // Sends a token-authenticated ARTEMIS command; `extra` is inserted
        // after cmdId.
        bool (*send_command)(void *context, std::uint32_t cmd_id,
                             const char *extra, const char *what);
        // Adds playback frame `seq` (subtype 0x02) to the next bulk ACK.
        void (*queue_ack)(void *context, std::uint16_t seq);
        // Writing `size` bytes is about to wait for the SD card: send the
        // pending ACK now.
        void (*before_sd_wait)(void *context, std::size_t size);
        // The stream advanced at `now_us` (a frame was taken).
        void (*progress)(void *context, std::int64_t now_us);
        // The current download ends: `stored` if the video is kept,
        // `stop_all` if the SD card failed.
        void (*finish_download)(void *context, bool stored, bool stop_all);
        // The SD writer has written a video that ended earlier: `stored` if
        // it is on the card; `sd_failed` if writing it failed.
        void (*pending_done)(void *context,
                             const camera::artemis::CatalogEntry &entry,
                             bool stored, bool sd_failed);
    };

    PlaybackDownload(const camera::artemis::Aes128 &cipher,
                     std::size_t reorder_frames, Host host);

    // Session start and end.
    void begin_session();
    // The session ended: drops the video being received; a finished one
    // waiting for the SD writer is kept (complete_pending(true) first).
    void end_session();

    bool active() const
    {
        return active_;
    }
    // True if a video stream is being received (data arrived).
    bool receiving() const
    {
        return active_ && last_data_us_ != 0;
    }
    std::int64_t started_us() const
    {
        return started_us_;
    }
    const camera::artemis::VideoTransferStats &stats() const
    {
        return stats_;
    }

    // Starts the download of `entry` (item `index` of `count`): opens its
    // two files and asks the camera to play it back. `camera_id` must stay
    // valid while the session runs. Returns false if the SD card cannot be
    // written.
    bool start(const camera::artemis::CatalogEntry &entry,
               const std::string &camera_id, std::size_t index,
               std::size_t count);

    // Handles one frame of the playback stream (bulk subtype 0x02). Returns
    // whether it advanced the stream (a new frame, not a repeat).
    bool handle_frame(std::uint16_t seq, const std::uint8_t *payload,
                      std::size_t payload_len);
    // A frame on another subtype during the video (leftovers of the
    // thumbnails, already acknowledged by the caller).
    void note_other_subtype_frame(std::uint8_t subtype, std::uint16_t seq,
                                  const std::uint8_t *payload,
                                  std::size_t payload_len);
    // A frame of a stopped stream arrived: wait a little longer before the
    // next video.
    void note_leftover_frame();

    // The camera's answer to the playback request (cmdId 769) for `entry`.
    enum class Response { started, retried, refused };
    Response handle_playback_response(const std::string &json,
                                      const camera::artemis::CatalogEntry &e);

    // True if the stream ended (the camera stopped sending).
    bool stream_ended(std::int64_t now_us) const;
    // Frames of a stopped stream may still be on their way: true until the
    // drain time has passed.
    bool draining(std::int64_t now_us) const;
    // True once the drain time after the last video has passed.
    bool drain_done(std::int64_t now_us) const;
    void clear_drain()
    {
        drain_until_us_ = 0;
    }

    // True if the ending video can be closed later, while the next one is
    // downloaded (complete_pending()).
    bool can_defer(bool stored) const;
    // Ends the video `entry`: stops the stream, logs and records its
    // figures, and either hands it to the SD writer (`deferred`) or closes
    // the audio file. Returns whether the video file is to be kept;
    // `video_bytes` receives the bytes received.
    bool finish(const camera::artemis::CatalogEntry &entry, bool stored,
                bool deferred, std::uint64_t *video_bytes);
    // The H.264 file of the current video (closed by the caller unless the
    // video was deferred).
    camera::store::MediaFileWriter &video_file();

    // Closes the files of the pending video once the SD writer has written
    // them (with `wait`, waits for that), and reports it (Host::pending_done).
    void complete_pending(bool wait);

    // Figures from the session loop and the bulk channel.
    void note_heartbeat();
    void note_loop_busy(std::int64_t busy_us);
    void note_ack_sent(std::uint8_t subtype, const std::uint16_t *seqs,
                       std::size_t count);
    void note_ack_failure()
    {
        stats_.ack_failures++;
    }

    // Logs where the time of the current video went.
    void log_transfer();

  private:
    // The H.264 and AAC files of a video. Two pairs take turns, so that the
    // next video is downloaded while the SD writer still writes the last
    // one.
    struct VideoFiles {
        camera::store::MediaFileWriter video;
        camera::store::MediaFileWriter audio;
    };
    // A downloaded video whose files are closed, and which is marked as
    // stored in the catalog, once the SD writer has written it.
    struct PendingVideo {
        bool active;
        camera::artemis::CatalogEntry entry;
        VideoFiles *files;
        std::uint32_t end;
        std::uint64_t bytes;
        std::int64_t started_us;
    };

    void send_playback_request(const camera::artemis::CatalogEntry &e);
    bool timed_write(camera::store::MediaFileWriter &file,
                     const std::uint8_t *data, std::size_t size);
    bool accept_frame(const camera::artemis::PlaybackFrame &frame);
    bool write_data(camera::artemis::PlaybackKind kind,
                    const std::uint8_t *data, std::size_t size);
    bool finish_frame(const camera::artemis::PlaybackDecoder::FrameInfo &info,
                      bool marker);
    void report_end(const camera::artemis::CatalogEntry &e, bool complete);
    void trace_start();
    void trace(char kind, std::uint16_t seq, std::int16_t value,
               std::uint8_t flag);
    void trace_write(const camera::artemis::CatalogEntry &e);

    Host host_;
    std::size_t reorder_frames_;
    const std::string *camera_id_ = nullptr;

    bool active_ = false;
    camera::artemis::PlaybackSession session_;
    std::int64_t file_type_ = 0;
    bool retried_ = false;
    std::int64_t total_frames_ = 0;
    std::int64_t last_data_us_ = 0;
    std::int64_t drain_until_us_ = 0;
    std::int64_t started_us_ = 0;
    bool write_failed_ = false;
    camera::artemis::VideoTransferStats stats_;

    camera::artemis::PlaybackStream playback_;
    camera::artemis::PlaybackDecoder decoder_;
    VideoFiles files_[2];
    VideoFiles *current_ = &files_[0];
    PendingVideo pending_ = {};

#if CONFIG_CAMERA_VIDEO_TRACE
    // Trace of the frames and ACKs (CONFIG_CAMERA_VIDEO_TRACE).
    struct TraceEvent {
        std::uint32_t time_us;
        std::uint16_t seq;
        std::int16_t value;
        char kind;
        std::uint8_t flag;
    };
    TraceEvent *trace_ = nullptr;
    std::size_t trace_count_ = 0;
    std::int64_t trace_started_us_ = 0;
    std::int16_t trace_acks_ = 0;
#endif
};

}  // namespace camera::link
