#include "playback_download.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED
#include "esp_wifi.h"
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
#include "esp_wifi_he.h"
#endif

#include "artemis_json.hpp"
#include "sd_writer.hpp"
#include "task_load.hpp"
#include "sdkconfig.h"

// Before the next video, wait until frames of the stopped stream stop coming.
#define CAMERA_VIDEO_DRAIN_US (1000 * 1000)
// Longest record accepted (the app buffers up to 2 MB per video frame); the
// records are decoded as they arrive, not collected.
#define CAMERA_VIDEO_MAX_RECORD (3u * 1024u * 1024u)
#define CAMERA_VIDEO_LOG_FRAMES 100

namespace camera::link
{

namespace
{

const char *TAG = "trailcamhub_protocol";

// Throughput test: receive and parse videos without writing them to the SD
// card (nothing is stored or marked as downloaded).
#ifdef CONFIG_CAMERA_VIDEO_DISCARD_DATA
constexpr bool kDiscardVideoData = true;
#else
constexpr bool kDiscardVideoData = false;
#endif

#if CONFIG_CAMERA_VIDEO_TRACE
constexpr std::size_t kTraceEvents = 65536;
#endif

}  // namespace

PlaybackDownload::PlaybackDownload(const camera::artemis::Aes128 &cipher,
                                   std::size_t reorder_frames, Host host)
    : host_(host), reorder_frames_(reorder_frames),
      playback_(reorder_frames, CAMERA_VIDEO_MAX_RECORD),
      decoder_(
          cipher,
          {[this](const camera::artemis::PlaybackFrame &frame) {
               return accept_frame(frame);
           },
           [this](camera::artemis::PlaybackKind kind, const std::uint8_t *data,
                  std::size_t size) { return write_data(kind, data, size); },
           [this](const camera::artemis::PlaybackDecoder::FrameInfo &info,
                  bool marker) { return finish_frame(info, marker); },
           [this]() { stats_.other_records++; }})
{
}

void PlaybackDownload::begin_session()
{
    active_ = false;
    drain_until_us_ = 0;
    playback_.reset();
    decoder_.reset();
}

void PlaybackDownload::end_session()
{
    if (current_->video.is_open()) {
        ESP_LOGW(TAG, "Session ended during a download; partial file removed");
        current_->video.abort();
    }
    current_->audio.abort();
    active_ = false;
}

void PlaybackDownload::send_playback_request(
    const camera::artemis::CatalogEntry &e)
{
    const std::string extra =
        ",\"fileType\":" + std::to_string(file_type_) +
        ",\"dirNum\":" + std::to_string(e.dir_num) +
        ",\"mediaNum\":" + std::to_string(e.media_num) +
        ",\"sessionNo\":" + std::to_string(session_.number());
    host_.send_command(host_.context, 769, extra.c_str(), "ARTEMIS StartPlayRecord");
}

bool PlaybackDownload::start(const camera::artemis::CatalogEntry &e,
                             const std::string &camera_id, std::size_t index,
                             std::size_t count)
{
    camera_id_ = &camera_id;
    const bool buffered = !camera::store::sd_writer_active();
    // The pair of files the pending video does not use.
    current_ =
        pending_.active && pending_.files == files_ ? &files_[1] : &files_[0];
    if (!current_->video.open(camera_id, e.media_id, "h264", buffered) ||
        !current_->audio.open(camera_id, e.media_id, "aac", buffered)) {
        current_->video.abort();
        current_->audio.abort();
        return false;
    }
    stats_.reset();
    session_.start(1 + esp_random() % 0xfffe);
    file_type_ = e.file_type;
    retried_ = false;
    total_frames_ = 0;
    last_data_us_ = 0;
    write_failed_ = false;
    camera::diag::task_load_start();
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
    static bool rx_statistics;
    if (!rx_statistics) {
        rx_statistics = esp_wifi_enable_rx_statistics(true, false) == ESP_OK;
    }
#endif
    // Drop what the SD writer measured before this video.
    std::int64_t card_us = 0;
    std::int64_t card_max_us = 0;
    camera::store::sd_writer_take_stats(card_us, card_max_us);
    playback_.reset();
    decoder_.reset();
    trace_start();
    active_ = true;
    ESP_LOGI(TAG,
             "Downloading video dir %lld num %lld (%u of %u) by playback, "
             "session %u",
             (long long)e.dir_num, (long long)e.media_num,
             (unsigned)(index + 1), (unsigned)count,
             (unsigned)session_.number());
    send_playback_request(e);
    started_us_ = esp_timer_get_time();
    return true;
}

PlaybackDownload::Response PlaybackDownload::handle_playback_response(
    const std::string &json, const camera::artemis::CatalogEntry &e)
{
    std::int64_t start = 0;
    std::int64_t result = 0;
    camera::artemis::json_find_int(json, "startPbRet", start);
    camera::artemis::json_find_int(json, "result", result);
    if (start == 0 && result == 0) {
        camera::artemis::json_find_int(json, "totalFrame", total_frames_);
        return Response::started;
    }
    std::int64_t other = 0;
    if (camera::artemis::next_playback_file_type(file_type_, e.file_type,
                                                 retried_, &other)) {
        ESP_LOGW(TAG,
                 "Camera refused playback with fileType %lld (startPbRet "
                 "%lld, result %lld); trying %lld",
                 (long long)file_type_, (long long)start, (long long)result,
                 (long long)other);
        retried_ = true;
        file_type_ = other;
        host_.progress(host_.context, esp_timer_get_time());
        send_playback_request(e);
        return Response::retried;
    }
    ESP_LOGW(TAG, "Camera refused playback (startPbRet %lld, result %lld)",
             (long long)start, (long long)result);
    return Response::refused;
}

bool PlaybackDownload::timed_write(camera::store::MediaFileWriter &file,
                                   const std::uint8_t *data, std::size_t size)
{
    stats_.bytes += size;
    if (kDiscardVideoData) {
        return true;
    }
    host_.before_sd_wait(host_.context, size);
    const std::int64_t started_us = esp_timer_get_time();
    const bool ok = camera::store::sd_writer_write(file, data, size);
    stats_.note_write(esp_timer_get_time() - started_us);
    return ok;
}

// A playback record's header is complete: accepts frames of the stream's
// session (PlaybackSession).
bool PlaybackDownload::accept_frame(const camera::artemis::PlaybackFrame &frame)
{
    using Verdict = camera::artemis::PlaybackSession::Verdict;
    const std::uint32_t requested = session_.number();
    const Verdict verdict =
        session_.check(frame.session_no, stats_.frames + stats_.audio_frames);
    if (verdict == Verdict::skip) {
        return false;
    }
    if (verdict == Verdict::adopt) {
        ESP_LOGW(TAG, "Camera uses session %u instead of %u; following it",
                 (unsigned)frame.session_no, (unsigned)requested);
    }
    return true;
}

// Appends decoded data to the H.264 or the AAC file.
bool PlaybackDownload::write_data(camera::artemis::PlaybackKind kind,
                                  const std::uint8_t *data, std::size_t size)
{
    const bool audio = kind == camera::artemis::PlaybackKind::audio;
    camera::store::MediaFileWriter &file =
        audio ? current_->audio : current_->video;
    (audio ? stats_.audio_bytes : stats_.media_bytes) += size;
    if (!timed_write(file, data, size)) {
        write_failed_ = true;
        return false;
    }
    return true;
}

// Counts a complete video or audio frame and logs the progress.
bool PlaybackDownload::finish_frame(
    const camera::artemis::PlaybackDecoder::FrameInfo &info, bool marker)
{
    const camera::artemis::PlaybackFrame &frame = info.frame;
    if (frame.kind == camera::artemis::PlaybackKind::audio) {
        stats_.audio_frames++;
        return true;
    }
    if (marker) {
        stats_.markers++;
        return true;
    }
    if (stats_.frames == 0) {
        ESP_LOGI(TAG, "First video frame: %ux%u, pts %u ms, %u bytes",
                 (unsigned)frame.width, (unsigned)frame.height,
                 (unsigned)frame.pts_ms, (unsigned)frame.size);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, info.first_bytes, info.first_size,
                                 ESP_LOG_INFO);
    }
    if (++stats_.frames % CAMERA_VIDEO_LOG_FRAMES == 0) {
        ESP_LOGI(TAG, "Video: %u of %lld frames, %llu KB",
                 (unsigned)stats_.frames, (long long)total_frames_,
                 (unsigned long long)(stats_.bytes / 1024));
        log_transfer();
    }
    return true;
}

bool PlaybackDownload::handle_frame(std::uint16_t seq,
                                    const std::uint8_t *payload,
                                    std::size_t payload_len)
{
    using camera::artemis::PlaybackStream;
#if CONFIG_CAMERA_VIDEO_TRACE
    {
        static const std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E',
                                                   'M', 'I', 'S', 0};
        const bool record_start =
            payload_len >= sizeof(kSignature) &&
            memcmp(payload, kSignature, sizeof(kSignature)) == 0;
        const std::int16_t delta =
            playback_.started()
                ? static_cast<std::int16_t>(seq - playback_.expected_seq())
                : 0;
        trace('R', seq, delta,
              static_cast<std::uint8_t>(
                  (record_start ? 1 : 0) |
                  (playback_.pending_frames() > 0 ? 2 : 0)));
    }
#endif
    // Frames beyond the reorder window: they show how far ahead of the first
    // missing frame the camera sends (and so how large the window must be).
    // Log the first ones and then a sample.
    if (playback_.started()) {
        const std::uint16_t expected = playback_.expected_seq();
        const std::int16_t delta = static_cast<std::int16_t>(seq - expected);
        const int window = static_cast<int>(reorder_frames_);
        if (delta > window || delta < -window) {
            if (delta < 0) {
                stats_.far_behind++;
            }
            if (stats_.far_logged < 40 || stats_.far_logged % 500 == 0) {
                ESP_LOGI(TAG,
                         "Playback frame seq %u, expected %u (delta %d), %u "
                         "bytes:",
                         (unsigned)seq, (unsigned)expected, (int)delta,
                         (unsigned)payload_len);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload,
                                         payload_len < 16 ? payload_len : 16,
                                         ESP_LOG_INFO);
            }
            stats_.far_logged++;
        }
    }
    const std::size_t pending_before = playback_.pending_frames();
    if (playback_.started()) {
        const std::int16_t behind =
            static_cast<std::int16_t>(playback_.expected_seq() - seq);
        if (behind > 0) {
            if (pending_before == 0) {
                stats_.repeats_without_gap++;
            }
            if (stats_.repeat_logged < 30) {
                ESP_LOGI(TAG,
                         "Video: seq %u repeated, %d behind the expected %u, "
                         "%u frames waiting",
                         (unsigned)seq, (int)behind,
                         (unsigned)playback_.expected_seq(),
                         (unsigned)pending_before);
            }
            stats_.repeat_logged++;
        }
    }
    bool ack = false;
    const PlaybackStream::Status status =
        playback_.feed(seq, payload, payload_len, decoder_, ack);
    const camera::artemis::VideoTransferStats::GapChange gap =
        stats_.note_pending(pending_before, playback_.pending_frames(), seq,
                            playback_.expected_seq(), esp_timer_get_time());
    if (gap.kind ==
        camera::artemis::VideoTransferStats::GapChange::Kind::closed) {
        if (stats_.gap_logged < 30) {
            ESP_LOGI(TAG,
                     "Video: gap at seq %u (%u missing) closed after %lld ms "
                     "by seq %u, %u frames had been waiting",
                     (unsigned)stats_.gap_seq, (unsigned)stats_.gap_missing,
                     (long long)(gap.open_us / 1000), (unsigned)seq,
                     (unsigned)pending_before);
        }
        stats_.gap_logged++;
    }
    if (ack) {
        host_.queue_ack(host_.context, seq);
        last_data_us_ = esp_timer_get_time();
        host_.progress(host_.context, last_data_us_);
    }
    if (status == PlaybackStream::Status::invalid) {
        if (write_failed_) {
            host_.finish_download(host_.context, false, true);
            return false;
        }
        ESP_LOGW(TAG, "Video stream out of sync after %llu frames",
                 (unsigned long long)playback_.frames());
        const std::size_t dump = payload_len < 24 ? payload_len : 24;
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, dump, ESP_LOG_WARN);
        // Keep what was written; the rest of the video is missing.
        host_.finish_download(host_.context, stats_.frames > 0, false);
        return false;
    }
    // Audio frames may still follow the last video frame, so the stream ends
    // only when the camera stops sending (see stream_ended()).
    if (total_frames_ > 0 &&
        static_cast<std::int64_t>(stats_.frames) == total_frames_ &&
        !stats_.complete_logged) {
        stats_.complete_logged = true;
        ESP_LOGI(TAG, "All %lld video frames received",
                 (long long)total_frames_);
    }
    return playback_.progressed();
}

void PlaybackDownload::note_other_subtype_frame(std::uint8_t subtype,
                                                std::uint16_t seq,
                                                const std::uint8_t *payload,
                                                std::size_t payload_len)
{
    if (stats_.other_subtype_frames < 5) {
        ESP_LOGI(TAG, "Video: frame on subtype 0x%02x, seq %u, %u bytes:",
                 (unsigned)subtype, (unsigned)seq, (unsigned)payload_len);
        ESP_LOG_BUFFER_HEX_LEVEL(
            TAG, payload, payload_len < 16 ? payload_len : 16, ESP_LOG_INFO);
    }
    stats_.other_subtype_frames++;
}

void PlaybackDownload::note_leftover_frame()
{
    if (drain_until_us_ != 0) {
        drain_until_us_ = esp_timer_get_time() + CAMERA_VIDEO_DRAIN_US;
    }
}

bool PlaybackDownload::stream_ended(std::int64_t now_us) const
{
    return camera::artemis::video_stream_ended(now_us, last_data_us_,
                                               stats_.frames, total_frames_);
}

bool PlaybackDownload::draining(std::int64_t now_us) const
{
    return drain_until_us_ > now_us;
}

bool PlaybackDownload::drain_done(std::int64_t now_us) const
{
    return drain_until_us_ != 0 && now_us >= drain_until_us_;
}

bool PlaybackDownload::can_defer(bool stored) const
{
    return active_ && stored && !kDiscardVideoData && stats_.frames > 0;
}

bool PlaybackDownload::finish(const camera::artemis::CatalogEntry &e,
                              bool stored, bool deferred,
                              std::uint64_t *video_bytes)
{
    const std::uint64_t bytes = stats_.bytes;
    // Stop the stream, also after it ended, as the app does.
    host_.send_command(host_.context, 770, "", "ARTEMIS StopPlayRecord");
    ESP_LOGI(
        TAG,
        "Video dir %lld num %lld: %u video frames (camera: %lld), %u "
        "audio frames, %u markers dropped, %u records of another "
        "session, %u other records, %llu + %llu bytes",
        (long long)e.dir_num, (long long)e.media_num, (unsigned)stats_.frames,
        (long long)total_frames_, (unsigned)stats_.audio_frames,
        (unsigned)stats_.markers, (unsigned)session_.foreign_records(),
        (unsigned)stats_.other_records, (unsigned long long)stats_.media_bytes,
        (unsigned long long)stats_.audio_bytes);
    log_transfer();
    stored = stored && stats_.frames > 0;
    report_end(e, stored);
    if (kDiscardVideoData) {
        const std::int64_t elapsed_ms =
            (esp_timer_get_time() - started_us_) / 1000;
        ESP_LOGI(TAG,
                 "Throughput test: %llu bytes in %lld ms, %llu KB/s; "
                 "video not stored",
                 (unsigned long long)bytes, (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0 ? bytes / elapsed_ms : 0));
        stored = false;
    }
    if (deferred) {
        // Only one video waits for the SD writer at a time.
        complete_pending(true);
        pending_ = {true,
                    e,
                    current_,
                    camera::store::sd_writer_end_files(&current_->video,
                                                       &current_->audio),
                    bytes,
                    started_us_};
    } else if (stored && current_->audio.size() > 0) {
        if (!current_->audio.finish(e.media_time)) {
            stored = false;
        }
    } else {
        current_->audio.abort();
    }
    active_ = false;
    playback_.reset();
    decoder_.reset();
    drain_until_us_ = esp_timer_get_time() + CAMERA_VIDEO_DRAIN_US;
    *video_bytes = bytes;
    return stored;
}

camera::store::MediaFileWriter &PlaybackDownload::video_file()
{
    return current_->video;
}

void PlaybackDownload::complete_pending(bool wait)
{
    if (!pending_.active) {
        return;
    }
    bool ok = true;
    while (!camera::store::sd_writer_ended(pending_.end, ok)) {
        if (!wait) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pending_.active = false;
    const camera::artemis::CatalogEntry &e = pending_.entry;
    VideoFiles &files = *pending_.files;
    bool stored = ok;
    if (stored && files.audio.size() > 0) {
        stored = files.audio.finish(e.media_time);
    } else {
        files.audio.abort();
    }
    if (stored && files.video.finish(e.media_time)) {
        const std::int64_t elapsed_ms =
            (esp_timer_get_time() - pending_.started_us) / 1000;
        ESP_LOGI(TAG,
                 "Stored %s (%llu bytes in %lld ms including the SD writes, "
                 "%llu KB/s)",
                 files.video.path().c_str(), (unsigned long long)pending_.bytes,
                 (long long)elapsed_ms,
                 (unsigned long long)(elapsed_ms > 0
                                          ? pending_.bytes / elapsed_ms
                                          : 0));
        host_.pending_done(host_.context, e, true, false);
        return;
    }
    files.video.abort();
    ESP_LOGW(TAG, "Download of dir %lld num %lld failed", (long long)e.dir_num,
             (long long)e.media_num);
    if (!ok) {
        ESP_LOGW(TAG, "SD card write failed; stopping downloads");
    }
    host_.pending_done(host_.context, e, false, !ok);
}

void PlaybackDownload::note_heartbeat()
{
    if (active_) {
        stats_.heartbeats++;
    }
}

void PlaybackDownload::note_loop_busy(std::int64_t busy_us)
{
    stats_.note_loop_busy(busy_us);
}

void PlaybackDownload::note_ack_sent(std::uint8_t subtype,
                                     const std::uint16_t *seqs,
                                     std::size_t count)
{
#if CONFIG_CAMERA_VIDEO_TRACE
    if (active_ && subtype == 0x02) {
        trace_acks_++;
        for (std::size_t i = 0; i < count; i++) {
            trace('A', seqs[i], trace_acks_, static_cast<std::uint8_t>(count));
        }
    }
#else
    (void)subtype;
    (void)seqs;
    (void)count;
#endif
}

// Logs where the time of the current video download went: gaps and repeats
// mean lost packets, long SD writes block the receive loop, low internal heap
// makes sends fail. Also logged with the progress and when the session ends
// during a video, so a stopped download still shows the counts.
void PlaybackDownload::log_transfer()
{
    const camera::artemis::FrameReorder::Stats &frames =
        playback_.reorder_stats();
#if SOC_WIFI_SUPPORTED
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        stats_.note_rssi(rssi);
    }
#endif
    ESP_LOGI(TAG,
             "Video transfer: %u frames missing in gaps (runs of 1: %u, 2-4: "
             "%u, 5-16: %u, more: %u), session loop busy up to %lld ms (%u "
             "times over 10 ms, %u over 50 ms), RSSI %d dBm (min %d)",
             (unsigned)stats_.missing, (unsigned)stats_.missing_runs[0],
             (unsigned)stats_.missing_runs[1], (unsigned)stats_.missing_runs[2],
             (unsigned)stats_.missing_runs[3],
             (long long)(stats_.loop_max_us / 1000),
             (unsigned)stats_.loop_over_10ms, (unsigned)stats_.loop_over_50ms,
             stats_.rssi_last, stats_.rssi_min);
    // Two lines: the SD log cuts lines at 256 characters.
    ESP_LOGI(TAG,
             "Video transfer: %u frames out of order, %u repeated by the "
             "camera (%u with no gap open), %u beyond the window, %u far "
             "behind, %u gaps (%lld ms on average, longest %lld ms)",
             (unsigned)frames.buffered, (unsigned)frames.duplicates,
             (unsigned)stats_.repeats_without_gap, (unsigned)frames.ahead,
             (unsigned)stats_.far_behind, (unsigned)stats_.gaps,
             (long long)stats_.gap_average_ms(),
             (long long)(stats_.gap_max_us / 1000));
    std::int64_t card_us = 0;
    std::int64_t card_max_us = 0;
    camera::store::sd_writer_take_stats(card_us, card_max_us);
    stats_.add_card_time(card_us, card_max_us);
    ESP_LOGI(TAG,
             "Video transfer: %u ACK send failures, waited for SD %lld ms "
             "(longest %lld ms), SD card busy %lld ms (longest %lld ms), "
             "internal heap %u B free (min %u B), %u heartbeats",
             (unsigned)stats_.ack_failures, (long long)(stats_.write_us / 1000),
             (long long)(stats_.write_max_us / 1000),
             (long long)(stats_.card_us / 1000),
             (long long)(stats_.card_max_us / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)stats_.heartbeats);
}

// Logs the CPU share of the tasks and the Wi-Fi driver's statistics for the
// video that ends, and appends a line with its figures to video_stats.csv,
// so that runs of different firmware can be compared.
void PlaybackDownload::report_end(const camera::artemis::CatalogEntry &e,
                                  bool complete)
{
    trace_write(e);
    const std::int64_t elapsed_ms = (esp_timer_get_time() - started_us_) / 1000;
    char tasks[160];
    const int idle = camera::diag::task_load_report(tasks, sizeof(tasks), 6);
    if (idle >= 0) {
        ESP_LOGI(TAG, "CPU during the video: idle %d%%, %s", idle, tasks);
    }
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
    esp_wifi_statis_dump(WIFI_STATIS_BUFFER | WIFI_STATIS_RXTX |
                         WIFI_STATIS_DIAG);
#endif

    const esp_app_desc_t *app = esp_app_get_description();
    camera::artemis::VideoEndInfo info;
    info.version = app->version;
    info.date = app->date;
    info.time = app->time;
    info.dir_num = e.dir_num;
    info.media_num = e.media_num;
    info.complete = complete;
    info.elapsed_ms = elapsed_ms;
    info.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    info.discard_test = kDiscardVideoData;
    info.cpu_idle_percent = idle;
    camera::store::append_line(*camera_id_, "video_stats.csv",
                               camera::artemis::kVideoStatsCsvHeader,
                               camera::artemis::video_stats_csv_line(
                                   stats_, playback_.reorder_stats(), info));
}

#if CONFIG_CAMERA_VIDEO_TRACE
// Trace of a video's frames and ACKs (CONFIG_CAMERA_VIDEO_TRACE): kind 'R' is
// a received frame (value: its distance to the expected sequence; flag bit 0:
// it starts an ARTEMIS record, bit 1: a gap was open), kind 'A' a sequence in
// a sent ACK (value: the ACK's number, flag: how many sequences it held).
void PlaybackDownload::trace_start()
{
    if (trace_ == nullptr) {
        trace_ = static_cast<TraceEvent *>(heap_caps_malloc(
            kTraceEvents * sizeof(TraceEvent), MALLOC_CAP_SPIRAM));
    }
    trace_count_ = 0;
    trace_acks_ = 0;
    trace_started_us_ = esp_timer_get_time();
}

void PlaybackDownload::trace(char kind, std::uint16_t seq, std::int16_t value,
                             std::uint8_t flag)
{
    if (trace_ != nullptr && trace_count_ < kTraceEvents) {
        trace_[trace_count_++] = {static_cast<std::uint32_t>(
                                      esp_timer_get_time() - trace_started_us_),
                                  seq, value, kind, flag};
    }
}

// Writes the trace to <camera>/trace_<mediaId>.csv.
void PlaybackDownload::trace_write(const camera::artemis::CatalogEntry &e)
{
    char name[48];
    snprintf(name, sizeof(name), "trace_%lld.csv", (long long)e.media_id);
    FILE *file = camera::store::create_camera_file(*camera_id_, name);
    if (trace_ == nullptr || file == nullptr) {
        if (file != nullptr) {
            fclose(file);
        }
        return;
    }
    fprintf(file, "time_us,kind,seq,value,flag\n");
    for (std::size_t i = 0; i < trace_count_; i++) {
        const TraceEvent &event = trace_[i];
        fprintf(file, "%lu,%c,%u,%d,%u\n", (unsigned long)event.time_us,
                event.kind, (unsigned)event.seq, (int)event.value,
                (unsigned)event.flag);
    }
    fclose(file);
    ESP_LOGI(TAG, "Video trace: %u events in %s%s", (unsigned)trace_count_,
             name, trace_count_ == kTraceEvents ? " (full)" : "");
}
#else
void PlaybackDownload::trace_start()
{
}
void PlaybackDownload::trace(char, std::uint16_t, std::int16_t, std::uint8_t)
{
}
void PlaybackDownload::trace_write(const camera::artemis::CatalogEntry &)
{
}
#endif

}  // namespace camera::link
