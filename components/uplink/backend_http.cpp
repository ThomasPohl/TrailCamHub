/*
 * HTTPS requests of the backend sync: the JSON POST with the device key and
 * the uploads of thumbnails (JPEG) and videos (MP4) to presigned URLs.
 */

#include "uplink.hpp"

#include <stdio.h>
#include <sys/time.h>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "backend_config.hpp"
#include "backend_internal.hpp"
#include "camera_actions.hpp"
#include "camera_wifi.h"
#include "delete_request.hpp"
#include "destinations.hpp"
#include "data_usage_store.hpp"
#include "device_info.hpp"
#include "cycle_trace.hpp"
#include "hub_battery.hpp"
#include "http_util.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "modem_uplink.hpp"
#include "mp4_body.hpp"
#include "mp4_mux.hpp"
#include "ota_policy.hpp"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "sync_cameras.hpp"
#include "system_diagnostics.h"
#include "time_sync.h"
#include "uplink_protocol.hpp"
#include "video_request.hpp"

namespace uplink::backend
{

namespace
{

const char *TAG = "trailcamhub_uplink";

}  // namespace

// POSTs a JSON body to the API with the device key.
// `body` is taken over and freed once it is sent, so that it does not sit
// next to the response.
HttpResult post_json(const std::string &url, const std::string &device_key,
                     std::string body)
{
    HttpResult result;
    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_POST);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    const std::string authorization = "Bearer " + device_key;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    const std::uint64_t sent_bytes = body.size();
    result.error = esp_http_client_open(client, body.size());
    if (result.error == ESP_OK) {
        if (esp_http_client_write(client, body.data(), body.size()) !=
            (int)body.size()) {
            result.error = ESP_FAIL;
        } else {
            std::string().swap(body);
            result = read_response(client);
        }
    }
    if (result.error != ESP_OK) {
        // While the connection still holds its memory: how much was left
        // for the response (#244).
        diagnostics_log_memory("request failed");
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    usage::record(sent_bytes, result.body.size());
    return result;
}

// PUTs a file to a presigned URL (no API credentials involved).
HttpResult put_file(const std::string &url, const std::string &path)
{
    HttpResult result;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.error = ESP_ERR_NOT_FOUND;
        return result;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_PUT);
    if (client == nullptr || size <= 0) {
        fclose(file);
        if (client != nullptr) {
            esp_http_client_cleanup(client);
        }
        result.error =
            client == nullptr ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_SIZE;
        return result;
    }
    // Must match the content type the URL was signed with.
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    result.error = esp_http_client_open(client, size);
    if (result.error == ESP_OK) {
        std::vector<char> chunk(kUploadChunk);
        size_t n;
        bool ok = true;
        while (ok && (n = fread(chunk.data(), 1, chunk.size(), file)) > 0) {
            ok = esp_http_client_write(client, chunk.data(), n) == (int)n;
        }
        result = ok ? read_response(client) : HttpResult{};
    }
    fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    usage::record((std::uint64_t)size, result.body.size());
    return result;
}

// PUTs a recording as MP4 to a presigned URL. The MP4 is built while it is
// sent, from the stored H.264 and AAC streams, so no copy is written.
// `too_large` is set to the reason (and nothing sent) if it exceeds the limit
// of the transport.
HttpResult put_mp4(const std::string &url, const std::string &camera_id,
                   std::int64_t media_id, bool over_modem, std::uint64_t &size,
                   std::string &too_large, std::int64_t &sd_read_us)
{
    HttpResult result;
    size = 0;
    sd_read_us = 0;
    too_large.clear();
    Mp4Body body(camera::store::media_path(camera_id, media_id, "h264"),
                 camera::store::media_path(camera_id, media_id, "aac"));
    const Mp4Status status = body.open();
    if (status == Mp4Status::video_missing) {
        result.error = ESP_ERR_NOT_FOUND;
        return result;
    }
    if (status != Mp4Status::ok) {
        ESP_LOGW(TAG, "Cannot build the MP4 of %s media %lld: %s",
                 camera_id.c_str(), (long long)media_id,
                 body.file.error().c_str());
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }
    mp4::Mp4File &file = body.file;
    size = file.size();
    VideoLimits limits;
    limits.wifi_bytes = kMaxVideoBytes;
    limits.modem_bytes = kMaxVideoBytesModem;
    too_large = video_size_failure(size, limits, over_modem);
    if (!too_large.empty()) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_PUT);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    // Must match the content type the URL was signed with.
    esp_http_client_set_header(client, "Content-Type", "video/mp4");
    // Modem sleep on the home Wi-Fi limits the upload to ~80 KB/s (#154).
    if (!over_modem) {
        camera_wifi_set_full_power(true);
    }
    result.error = esp_http_client_open(client, (int)size);
    if (result.error == ESP_OK) {
        std::vector<std::uint8_t> chunk(kVideoChunk);
        std::uint64_t position = 0;
        bool ok = true;
        while (ok && position < size) {
            const std::int64_t read_started_us = esp_timer_get_time();
            const std::int64_t n =
                file.read(position, chunk.data(), chunk.size());
            sd_read_us += esp_timer_get_time() - read_started_us;
            ok = n > 0 &&
                 esp_http_client_write(
                     client, reinterpret_cast<const char *>(chunk.data()),
                     (int)n) == (int)n;
            position += n > 0 ? (std::uint64_t)n : 0;
        }
        result = ok ? read_response(client) : HttpResult{};
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    usage::record(size, result.body.size());
    if (!over_modem) {
        camera_wifi_set_full_power(false);
    }
    return result;
}

}  // namespace uplink::backend
