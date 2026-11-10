/*
 * The destinations besides the own backend (docs/modes.md):
 *
 *   WebDAV:   the hub copies new thumbnails, photos and videos into a
 *             folder of a NAS or cloud (Nextcloud, Synology, ...), one
 *             subfolder per camera.
 *   Telegram: the hub talks to the Bot API itself (only over the modem, see
 *             #150): new thumbnails as photos with a "Get video" button,
 *             requested videos as MP4, and the commands /scan and /status.
 *
 * Both keep track of what was sent in the catalog: kReported for a
 * thumbnail, kVideoUploaded for a photo or video file.
 */

#include "destinations.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_wifi.h"
#include "device_info.hpp"
#include "http_util.hpp"
#include "hub_battery.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "mp4_file_source.hpp"
#include "mp4_mux.hpp"
#include "push_protocol.hpp"
#include "schedule_state.hpp"
#include "sdkconfig.h"

namespace uplink
{

namespace
{

const char *TAG = "trailcamhub_push";

using camera::artemis::CatalogEntry;
using camera::artemis::MediaCatalog;

#if CONFIG_CAMERA_DOWNLOAD_VIDEOS
constexpr bool kDownloadsVideos = true;
#else
constexpr bool kDownloadsVideos = false;
#endif

bool has(const CatalogEntry &entry, MediaCatalog::Flag flag)
{
    return (entry.flags & flag) != 0;
}

bool is_video(const CatalogEntry &entry)
{
    return entry.file_type == 2;
}

// A recording's MP4, built while it is read from the stored H.264 and AAC.
struct Mp4Body {
    mp4::FileSource video;
    mp4::FileSource audio;
    mp4::Mp4File file;
    Mp4Body(const std::string &camera_id, std::int64_t media_id)
        : video(camera::store::media_path(camera_id, media_id, "h264")),
          audio(camera::store::media_path(camera_id, media_id, "aac"))
    {
    }
};

// Opens the MP4 of a recording; nullptr (and `reason`) if it cannot be built.
std::shared_ptr<Mp4Body> open_mp4(const std::string &camera_id,
                                  std::int64_t media_id, std::string &reason)
{
    auto body = std::make_shared<Mp4Body>(camera_id, media_id);
    if (!body->video.exists()) {
        reason = "the video is not on the SD card";
        return nullptr;
    }
    if (!body->file.open(body->video,
                         body->audio.exists() && body->audio.size() > 0
                             ? &body->audio
                             : nullptr)) {
        reason = "the MP4 cannot be built: " + body->file.error();
        return nullptr;
    }
    return body;
}

BodyReader mp4_reader(const std::shared_ptr<Mp4Body> &body)
{
    return [body](std::uint64_t offset, std::uint8_t *buffer,
                  std::size_t size) -> std::int64_t {
        return body->file.read(offset, buffer, size);
    };
}

// A body of head + reader + tail (a multipart file upload).
BodyReader framed(const std::string &head, BodyReader inner,
                  std::uint64_t inner_size, const std::string &tail)
{
    auto h = std::make_shared<std::string>(head);
    auto t = std::make_shared<std::string>(tail);
    return [h, t, inner, inner_size](std::uint64_t offset, std::uint8_t *buffer,
                                     std::size_t size) -> std::int64_t {
        if (offset < h->size()) {
            const std::size_t n =
                std::min<std::size_t>(size, h->size() - (std::size_t)offset);
            std::copy(h->data() + offset, h->data() + offset + n, buffer);
            return (std::int64_t)n;
        }
        offset -= h->size();
        if (offset < inner_size) {
            const std::size_t n =
                (std::size_t)std::min<std::uint64_t>(size, inner_size - offset);
            return inner(offset, buffer, n);
        }
        offset -= inner_size;
        if (offset >= t->size()) {
            return 0;
        }
        const std::size_t n =
            std::min<std::size_t>(size, t->size() - (std::size_t)offset);
        std::copy(t->data() + offset, t->data() + offset + n, buffer);
        return (std::int64_t)n;
    };
}

struct CameraFiles {
    std::string id;
    MediaCatalog catalog;
    bool changed = false;
};

std::vector<CameraFiles> load_cameras()
{
    std::vector<CameraFiles> cameras;
    for (const std::string &id : camera::store::list_cameras()) {
        CameraFiles camera;
        camera.id = id;
        camera::store::load_catalog(id, camera.catalog);
        cameras.push_back(std::move(camera));
    }
    return cameras;
}

void save_cameras(std::vector<CameraFiles> &cameras)
{
    for (CameraFiles &camera : cameras) {
        if (camera.changed &&
            !camera::store::save_catalog(camera.id, camera.catalog)) {
            ESP_LOGW(TAG, "Could not save the catalog of %s",
                     camera.id.c_str());
        }
        camera.changed = false;
    }
}

bool success(const HttpResult &result)
{
    return result.error == ESP_OK && result.status >= 200 &&
           result.status < 300;
}

// --- WebDAV ---

struct WebDav {
    const hub_config::Settings &settings;

    Request request(const std::vector<std::string> &segments,
                    esp_http_client_method_t method) const
    {
        Request r;
        r.url = push::join_url(settings.webdav_url, segments);
        r.method = method;
        r.user = settings.webdav_user;
        r.password = settings.webdav_password;
        return r;
    }

    // Creates a folder; one that exists already (405) is fine.
    bool make_folder(const std::vector<std::string> &segments,
                     std::string &error) const
    {
        const HttpResult result = send(request(segments, HTTP_METHOD_MKCOL));
        if (success(result) || result.status == 405) {
            return true;
        }
        error = "WebDAV folder " + push::join_url("", segments) + ": " +
                describe(result);
        return false;
    }

    bool put(const std::vector<std::string> &segments, const char *type,
             BodyReader body, std::uint64_t size, std::string &error,
             std::int64_t *read_us = nullptr) const
    {
        Request r = request(segments, HTTP_METHOD_PUT);
        r.headers.push_back({"Content-Type", type});
        r.body = std::move(body);
        r.body_size = size;
        const HttpResult result = send(r, read_us);
        if (success(result)) {
            return true;
        }
        error = "WebDAV upload " + push::join_url("", segments) + ": " +
                describe(result);
        return false;
    }

    bool put_file(const std::vector<std::string> &segments, const char *type,
                  const std::string &path, std::string &error) const
    {
        BodyReader body;
        std::uint64_t size = 0;
        if (file_body(path, body, size) != ESP_OK) {
            error = "missing on the SD card: " + path;
            return false;
        }
        return put(segments, type, body, size, error);
    }
};

}  // namespace

esp_err_t push_webdav(const hub_config::Settings &settings, bool over_modem,
                      Report &report)
{
    const WebDav dav{settings};
    std::vector<CameraFiles> cameras = load_cameras();
    std::string error;
    // A folder for the hub's files may not exist yet.
    if (!dav.make_folder({}, error)) {
        report.error = error;
        ESP_LOGE(TAG, "%s", error.c_str());
        return ESP_FAIL;
    }
    for (CameraFiles &camera : cameras) {
        report.cameras_reported++;
        bool folders = false;
        auto ensure_folders = [&]() {
            if (!folders) {
                folders = dav.make_folder({camera.id}, error) &&
                          dav.make_folder({camera.id, "thumb"}, error) &&
                          dav.make_folder({camera.id, "media"}, error);
            }
            return folders;
        };
        for (const CatalogEntry &entry : camera.catalog.entries()) {
            const char *ext = is_video(entry) ? "mp4" : "jpg";
            if (has(entry, MediaCatalog::kThumbPresent) &&
                !has(entry, MediaCatalog::kReported)) {
                if (!ensure_folders() ||
                    !dav.put_file({camera.id, "thumb",
                                   push::file_name(entry.media_time,
                                                   entry.media_id, "jpg")},
                                  "image/jpeg",
                                  camera::store::thumbnail_path(camera.id,
                                                                entry.media_id),
                                  error)) {
                    break;
                }
                camera.changed |= camera.catalog.set_flag(
                    entry.media_id, MediaCatalog::kReported);
                report.thumbs_uploaded++;
            }
            if (!has(entry, MediaCatalog::kFileDownloaded) ||
                has(entry, MediaCatalog::kVideoUploaded)) {
                continue;
            }
            const std::string name =
                push::file_name(entry.media_time, entry.media_id, ext);
            if (!is_video(entry)) {
                if (!ensure_folders() ||
                    !dav.put_file({camera.id, "media", name}, "image/jpeg",
                                  camera::store::media_path(
                                      camera.id, entry.media_id, "jpg"),
                                  error)) {
                    break;
                }
            } else {
                std::string reason;
                auto mp4 = open_mp4(camera.id, entry.media_id, reason);
                if (mp4 == nullptr) {
                    ESP_LOGW(TAG, "Video %lld of %s: %s",
                             (long long)entry.media_id, camera.id.c_str(),
                             reason.c_str());
                    continue;
                }
                if (mp4->file.size() > max_video_bytes(over_modem)) {
                    ESP_LOGW(TAG, "Video %lld of %s: %llu KB, too large here",
                             (long long)entry.media_id, camera.id.c_str(),
                             (unsigned long long)(mp4->file.size() >> 10));
                    continue;
                }
                const std::int64_t started_us = esp_timer_get_time();
                std::int64_t read_us = 0;
                if (!over_modem) {
                    camera_wifi_set_full_power(true);
                }
                const bool sent =
                    ensure_folders() &&
                    dav.put({camera.id, "media", name}, "video/mp4",
                            mp4_reader(mp4), mp4->file.size(), error, &read_us);
                if (!over_modem) {
                    camera_wifi_set_full_power(false);
                }
                if (!sent) {
                    break;
                }
                const std::int64_t ms =
                    (esp_timer_get_time() - started_us) / 1000;
                ESP_LOGI(TAG,
                         "Copied video %lld of %s to WebDAV: %llu KB in %lld "
                         "ms (reading the SD card %lld ms)",
                         (long long)entry.media_id, camera.id.c_str(),
                         (unsigned long long)(mp4->file.size() >> 10),
                         (long long)ms, (long long)(read_us / 1000));
                report.videos_uploaded++;
            }
            camera.changed |= camera.catalog.set_flag(
                entry.media_id, MediaCatalog::kVideoUploaded);
        }
        if (error.empty() && ensure_folders()) {
            // The camera's state (battery, SD card) next to its pictures.
            dav.put_file({camera.id, "camera.json"}, "application/json",
                         camera::store::camera_info_path(camera.id), error);
        }
        if (!error.empty()) {
            break;
        }
    }
    save_cameras(cameras);
    if (!error.empty()) {
        report.error = error;
        ESP_LOGE(TAG, "%s", error.c_str());
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WebDAV: %d cameras, %d thumbnails and %d videos copied",
             report.cameras_reported, report.thumbs_uploaded,
             report.videos_uploaded);
    return ESP_OK;
}

// --- Telegram ---

namespace
{

constexpr const char *kOffsetKey = "tg_offset";
// Thumbnails sent per check-in; the rest follow with the next one (and
// Telegram limits a bot to about one message per second and chat).
constexpr int kMaxPhotosPerSync = 20;
constexpr const char *kBoundary = "TrailCamHubBoundary7MA4YWxkTrZu0gW";

class Telegram
{
  public:
    explicit Telegram(const hub_config::Settings &settings)
        : api_("https://api.telegram.org/bot" + settings.telegram_token + "/")
    {
    }

    HttpResult call(const std::string &method, const std::string &json) const
    {
        Request r;
        r.url = api_ + method;
        r.method = HTTP_METHOD_POST;
        r.headers.push_back({"Content-Type", "application/json"});
        r.body = string_body(json);
        r.body_size = json.size();
        return send(r);
    }

    bool message(std::int64_t chat, const std::string &text) const
    {
        const HttpResult result =
            call("sendMessage", "{\"chat_id\":" + std::to_string(chat) +
                                    ",\"text\":" + quote(text) + "}");
        return success(result) && push::api_ok(result.body);
    }

    void answer(const std::string &callback_id, const std::string &text) const
    {
        call("answerCallbackQuery",
             "{\"callback_query_id\":" + quote(callback_id) +
                 ",\"text\":" + quote(text) + "}");
    }

    // sendPhoto / sendVideo with a file from the SD card or an MP4.
    HttpResult upload(const std::string &method, const std::string &field,
                      std::int64_t chat,
                      std::vector<std::pair<std::string, std::string>> fields,
                      const std::string &file_name, const char *type,
                      BodyReader file, std::uint64_t size) const
    {
        fields.insert(fields.begin(), {"chat_id", std::to_string(chat)});
        const std::string head =
            push::multipart_head(kBoundary, fields, field, file_name, type);
        const std::string tail = push::multipart_tail(kBoundary);
        Request r;
        r.url = api_ + method;
        r.method = HTTP_METHOD_POST;
        r.headers.push_back(
            {"Content-Type",
             std::string("multipart/form-data; boundary=") + kBoundary});
        r.body = framed(head, std::move(file), size, tail);
        r.body_size = head.size() + size + tail.size();
        return send(r);
    }

    static std::string quote(const std::string &text)
    {
        std::string out = "\"";
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += (char)c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c >= 0x20) {
                out += (char)c;
            }
        }
        return out + "\"";
    }

    // The URL of getUpdates from `offset` (only messages and button presses).
    std::string updates_url(std::int64_t offset) const
    {
        return api_ + "getUpdates?timeout=0&offset=" +
               std::to_string((long long)offset) +
               "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";
    }

  private:
    std::string api_;
};

std::string status_text(const std::vector<CameraFiles> &cameras)
{
    const schedule::Effective effective = schedule::effective();
    const schedule::Times times = schedule::next_times(effective);
    const std::int64_t now = schedule::now();
    std::string text = "TrailCamHub";
    const int battery = hub_battery::percent();
    if (battery >= 0) {
        text += "\nHub battery " + std::to_string(battery) + " %";
    }
    for (const CameraFiles &camera : cameras) {
        camera::artemis::DeviceInfo info;
        std::int64_t read_at = 0;
        if (camera::store::load_camera_info(camera.id, info, read_at)) {
            const std::string state =
                camera::artemis::device_state_summary(info);
            // The camera's own name is often a default ("0000"), so the
            // directory id names it.
            text += "\nCamera " + camera.id + ": " +
                    (state.empty() ? "" : state + ", ") +
                    std::to_string(camera.catalog.size()) + " recordings";
        }
    }
    auto minutes = [now](std::int64_t t) {
        return t == schedule::kNever
                   ? std::string("never")
                   : "in " +
                         std::to_string(
                             (long long)((t > now ? t - now : 0) / 60)) +
                         " min";
    };
    text += "\nNext camera read " + minutes(times.camera);
    text += "\nNext check-in " + minutes(times.checkin) +
            " (the hub answers buttons and commands then)";
    return text;
}

CameraFiles *find(std::vector<CameraFiles> &cameras, const std::string &id)
{
    for (CameraFiles &camera : cameras) {
        if (camera.id == id) {
            return &camera;
        }
    }
    return nullptr;
}

}  // namespace

esp_err_t push_telegram(const hub_config::Settings &settings, bool over_modem,
                        Report &report)
{
    const Telegram bot(settings);
    std::vector<CameraFiles> cameras = load_cameras();
    std::int64_t chat =
        settings.telegram_chat.empty()
            ? 0
            : std::strtoll(settings.telegram_chat.c_str(), nullptr, 10);

    // 1. Commands and button presses since the last check-in.
    std::int64_t offset =
        std::strtoll(hub_config::load_state(kOffsetKey).c_str(), nullptr, 10);
    Request poll;
    poll.url = bot.updates_url(offset);
    const HttpResult polled = send(poll);
    std::vector<push::Update> updates;
    if (!success(polled) || !push::parse_updates(polled.body, updates)) {
        report.error = "Telegram getUpdates: " + describe(polled);
        if (polled.status == 401 || polled.status == 404) {
            report.error += " (bot token rejected)";
        } else if (polled.status == 409) {
            report.error += " (the bot has a webhook or another client "
                            "reads its updates: use a bot of its own)";
        }
        ESP_LOGE(TAG, "%s", report.error.c_str());
        return ESP_FAIL;
    }
    bool read_requested = false;
    for (const push::Update &update : updates) {
        if (update.update_id >= offset) {
            offset = update.update_id + 1;
        }
        const std::string command = push::command_of(update.text);
        if (chat == 0 && command == "/start" && update.chat_id != 0) {
            // The first chat that sends /start is the only one.
            chat = update.chat_id;
            hub_config::set(hub_config::Key::telegram_chat,
                            std::to_string((long long)chat), nullptr);
            bot.message(chat, "TrailCamHub is paired with this chat. New "
                              "pictures arrive with each check-in; /status "
                              "and /scan are answered then too.");
            ESP_LOGI(TAG, "Paired with Telegram chat %lld", (long long)chat);
            continue;
        }
        if (update.chat_id != chat || chat == 0) {
            if (!update.callback_id.empty()) {
                bot.answer(update.callback_id, "Not paired with this chat");
            }
            continue;
        }
        report.commands_received++;
        if (!update.callback_id.empty()) {
            std::string camera_id;
            std::int64_t media_id = 0;
            CameraFiles *camera = nullptr;
            if (push::parse_video_button(update.callback_data, camera_id,
                                         media_id)) {
                camera = find(cameras, camera_id);
            }
            if (camera == nullptr ||
                camera->catalog.find(media_id) == nullptr) {
                bot.answer(update.callback_id, "Unknown recording");
                continue;
            }
            camera->changed |= camera->catalog.set_flag(
                media_id, MediaCatalog::kVideoRequested);
            read_requested = true;
            bot.answer(update.callback_id,
                       "Video requested; it follows after the next camera "
                       "read");
        } else if (command == "/scan") {
            read_requested = true;
            bot.message(chat, "Reading the cameras now; new pictures follow "
                              "right after.");
        } else if (command == "/status") {
            bot.message(chat, status_text(cameras));
        } else if (command == "/start" || command == "/help") {
            bot.message(chat, "/status: battery and schedule\n/scan: read "
                              "the cameras now\nThe button under a video "
                              "fetches it.");
        }
    }
    hub_config::store_state(kOffsetKey, std::to_string((long long)offset));
    if (read_requested) {
        schedule::request_camera_read("asked for in Telegram");
    }
    if (chat == 0) {
        save_cameras(cameras);
        ESP_LOGW(TAG, "No Telegram chat yet: send /start to the bot");
        return ESP_OK;
    }

    // 2. New thumbnails, with a button for the video of a video.
    int photos = 0;
    int waiting = 0;
    std::string error;
    for (CameraFiles &camera : cameras) {
        report.cameras_reported++;
        for (const CatalogEntry &entry : camera.catalog.entries()) {
            if (!has(entry, MediaCatalog::kThumbPresent) ||
                has(entry, MediaCatalog::kReported) || !error.empty()) {
                continue;
            }
            if (photos >= kMaxPhotosPerSync) {
                waiting++;
                continue;
            }
            BodyReader body;
            std::uint64_t size = 0;
            if (file_body(
                    camera::store::thumbnail_path(camera.id, entry.media_id),
                    body, size) != ESP_OK) {
                continue;
            }
            std::vector<std::pair<std::string, std::string>> fields;
            const std::string when = push::wall_time(entry.media_time);
            fields.push_back(
                {"caption", camera.id + (when.empty() ? "" : " · " + when) +
                                (is_video(entry) ? " · video" : "")});
            if (is_video(entry) && kDownloadsVideos) {
                fields.push_back(
                    {"reply_markup",
                     push::inline_keyboard(
                         "Get video",
                         push::video_button(camera.id, entry.media_id))});
            }
            const HttpResult sent = bot.upload(
                "sendPhoto", "photo", chat, fields,
                push::file_name(entry.media_time, entry.media_id, "jpg"),
                "image/jpeg", body, size);
            if (!success(sent) || !push::api_ok(sent.body)) {
                error = "Telegram sendPhoto: " + describe(sent);
                continue;
            }
            camera.changed |= camera.catalog.set_flag(entry.media_id,
                                                      MediaCatalog::kReported);
            photos++;
            report.thumbs_uploaded++;
            // About one message per second and chat.
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (waiting > 0 && error.empty()) {
        bot.message(chat, std::to_string(waiting) +
                              " more pictures follow with the next check-in.");
    }

    // 3. Requested videos that are on the SD card now; failed ones are
    // answered and forgotten, so that a new request starts over.
    for (CameraFiles &camera : cameras) {
        for (const CatalogEntry &entry : camera.catalog.entries()) {
            if (!error.empty() || !has(entry, MediaCatalog::kVideoRequested) ||
                has(entry, MediaCatalog::kVideoUploaded)) {
                continue;
            }
            const std::string label =
                camera.id + " " + push::wall_time(entry.media_time);
            if (has(entry, MediaCatalog::kVideoUnavailable) ||
                camera.catalog.video_attempts(entry.media_id) >=
                    MediaCatalog::kMaxVideoAttempts ||
                !kDownloadsVideos) {
                bot.message(chat, "The video " + label +
                                      " could not be fetched from the camera.");
                camera.changed |=
                    camera.catalog.forget_video_request(entry.media_id);
                continue;
            }
            if (!has(entry, MediaCatalog::kFileDownloaded)) {
                continue;  // with the next camera read
            }
            std::string reason;
            auto mp4 = open_mp4(camera.id, entry.media_id, reason);
            if (mp4 != nullptr &&
                mp4->file.size() > max_video_bytes(over_modem)) {
                reason = "it is too large for this uplink (" +
                         std::to_string(
                             (unsigned long long)(mp4->file.size() >> 20)) +
                         " MB)";
                mp4 = nullptr;
            }
            if (mp4 == nullptr) {
                bot.message(chat, "The video " + label +
                                      " cannot be sent: " + reason);
                camera.changed |=
                    camera.catalog.forget_video_request(entry.media_id);
                continue;
            }
            const std::int64_t started_us = esp_timer_get_time();
            const HttpResult sent = bot.upload(
                "sendVideo", "video", chat,
                {{"caption", label}, {"supports_streaming", "true"}},
                push::file_name(entry.media_time, entry.media_id, "mp4"),
                "video/mp4", mp4_reader(mp4), mp4->file.size());
            if (!success(sent) || !push::api_ok(sent.body)) {
                error = "Telegram sendVideo: " + describe(sent);
                continue;
            }
            ESP_LOGI(TAG,
                     "Sent video %lld of %s to Telegram: %llu KB in %lld "
                     "ms",
                     (long long)entry.media_id, camera.id.c_str(),
                     (unsigned long long)(mp4->file.size() >> 10),
                     (long long)((esp_timer_get_time() - started_us) / 1000));
            camera.changed |= camera.catalog.set_flag(
                entry.media_id, MediaCatalog::kVideoUploaded);
            report.videos_uploaded++;
        }
    }
    save_cameras(cameras);
    if (!error.empty()) {
        report.error = error;
        ESP_LOGE(TAG, "%s", error.c_str());
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Telegram: %d pictures and %d videos sent, %d commands",
             report.thumbs_uploaded, report.videos_uploaded,
             report.commands_received);
    return ESP_OK;
}

}  // namespace uplink
