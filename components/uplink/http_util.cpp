#include "http_util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace uplink
{

namespace
{

constexpr int kTimeoutMs = 20000;
constexpr int kMaxResponseBytes = 64 * 1024;
constexpr std::size_t kChunk = 16 * 1024;
// A response without Content-Length grows in steps of this size.
constexpr std::size_t kBodyStep = 4 * 1024;

const char *TAG = "trailcamhub_http";

// Makes room for `capacity` bytes in `body`, or returns false if the heap
// has no block for it. C++ exceptions are off, so a std::string that cannot
// grow aborts (the WROOM-32D did, while it read a check-in response over
// TLS); malloc() says beforehand whether the block is there. Another task
// can take it in between, which the probe makes unlikely, not impossible.
bool reserve_body(std::string &body, std::size_t capacity)
{
    if (body.capacity() >= capacity) {
        return true;
    }
    void *probe = malloc(capacity + 1);
    if (probe == nullptr) {
        ESP_LOGW(TAG,
                 "No %u B block for the response (largest free block %u B, "
                 "free heap %u B)",
                 (unsigned)(capacity + 1),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        return false;
    }
    free(probe);
    body.reserve(capacity);
    return true;
}

// Switches the log of ESP-IDF's HTTP client off while it exists and restores
// the level it had. The level is global, so a request in another task is
// quiet meanwhile too; the destinations run one at a time in the uplink task.
class QuietClientLog
{
  public:
    explicit QuietClientLog(bool active) : active_(active)
    {
        if (active_) {
            previous_ = esp_log_level_get(kTag);
            esp_log_level_set(kTag, ESP_LOG_NONE);
        }
    }
    ~QuietClientLog()
    {
        if (active_) {
            esp_log_level_set(kTag, previous_);
        }
    }
    QuietClientLog(const QuietClientLog &) = delete;
    QuietClientLog &operator=(const QuietClientLog &) = delete;

  private:
    static constexpr const char *kTag = "HTTP_CLIENT";  // esp_http_client.c
    bool active_;
    esp_log_level_t previous_ = ESP_LOG_INFO;
};

}  // namespace

esp_http_client_handle_t open_client(const std::string &url,
                                     esp_http_client_method_t method)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    // Presigned S3 URLs are long; the request line must fit.
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = false;
    return esp_http_client_init(&config);
}

HttpResult read_response(esp_http_client_handle_t client)
{
    HttpResult result;
    if (esp_http_client_fetch_headers(client) < 0) {
        result.error = ESP_ERR_HTTP_FETCH_HEADER;
        return result;
    }
    result.status = esp_http_client_get_status_code(client);
    // With a Content-Length the body gets one block of its size. A string
    // that grows by itself doubles and copies: a 20 KB response then needs
    // a 32 KB block next to the 16 KB one it already holds.
    const std::int64_t length = esp_http_client_get_content_length(client);
    if (length > kMaxResponseBytes) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    if (length > 0 && !reserve_body(result.body, (std::size_t)length)) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    char buffer[512];
    int read;
    while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        const std::size_t needed = result.body.size() + (std::size_t)read;
        if (needed > (std::size_t)kMaxResponseBytes) {
            result.error = ESP_ERR_INVALID_SIZE;
            return result;
        }
        if (needed > result.body.capacity() &&
            !reserve_body(result.body, needed + kBodyStep)) {
            result.error = ESP_ERR_NO_MEM;
            return result;
        }
        result.body.append(buffer, read);
    }
    result.error = read < 0 ? ESP_FAIL : ESP_OK;
    return result;
}

std::string describe(const HttpResult &result)
{
    if (result.error != ESP_OK) {
        return esp_err_to_name(result.error);
    }
    return "HTTP " + std::to_string(result.status) + " " +
           result.body.substr(0, 120);
}

HttpResult send(const Request &request, std::int64_t *read_us)
{
    HttpResult result;
    const QuietClientLog quiet(request.secret_url);
    esp_http_client_handle_t client = open_client(request.url, request.method);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    for (const auto &header : request.headers) {
        esp_http_client_set_header(client, header.first.c_str(),
                                   header.second.c_str());
    }
    if (!request.user.empty()) {
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
        esp_http_client_set_username(client, request.user.c_str());
        esp_http_client_set_password(client, request.password.c_str());
    }
    result.error = esp_http_client_open(client, (int)request.body_size);
    if (result.error == ESP_OK) {
        bool ok = true;
        bool no_memory = false;
        if (request.body_size > 0) {
            // A small body (a check-in) does not need the 16 KB of a video
            // upload, and a board without PSRAM may not have them.
            const std::size_t chunk_size = request.body_size < kChunk
                                               ? (std::size_t)request.body_size
                                               : kChunk;
            std::unique_ptr<std::uint8_t[]> chunk(new (std::nothrow)
                                                      std::uint8_t[chunk_size]);
            if (!chunk) {
                ESP_LOGW(TAG, "No %u B block to send the request body",
                         (unsigned)chunk_size);
                no_memory = true;
                ok = false;
            }
            std::uint64_t position = 0;
            while (ok && position < request.body_size) {
                std::size_t want = chunk_size;
                if (request.body_size - position < want) {
                    want = (std::size_t)(request.body_size - position);
                }
                const std::int64_t started_us = esp_timer_get_time();
                const std::int64_t n =
                    request.body ? request.body(position, chunk.get(), want)
                                 : -1;
                if (read_us != nullptr) {
                    *read_us += esp_timer_get_time() - started_us;
                }
                ok = n > 0 &&
                     esp_http_client_write(
                         client, reinterpret_cast<const char *>(chunk.get()),
                         (int)n) == (int)n;
                position += n > 0 ? (std::uint64_t)n : 0;
            }
        }
        if (ok) {
            result = read_response(client);
        } else {
            result = HttpResult{};
            if (no_memory) {
                result.error = ESP_ERR_NO_MEM;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

BodyReader string_body(const std::string &text)
{
    auto data = std::make_shared<std::string>(text);
    return [data](std::uint64_t offset, std::uint8_t *buffer,
                  std::size_t size) -> std::int64_t {
        if (offset >= data->size()) {
            return 0;
        }
        const std::size_t n =
            std::min<std::size_t>(size, data->size() - offset);
        std::copy(data->data() + offset, data->data() + offset + n, buffer);
        return (std::int64_t)n;
    };
}

esp_err_t file_body(const std::string &path, BodyReader &reader,
                    std::uint64_t &size)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    size = (std::uint64_t)length;
    std::shared_ptr<std::FILE> handle(file,
                                      [](std::FILE *f) { std::fclose(f); });
    reader = [handle](std::uint64_t offset, std::uint8_t *buffer,
                      std::size_t want) -> std::int64_t {
        if (std::fseek(handle.get(), (long)offset, SEEK_SET) != 0) {
            return -1;
        }
        const std::size_t n = std::fread(buffer, 1, want, handle.get());
        return n > 0 ? (std::int64_t)n : -1;
    };
    return ESP_OK;
}

std::uint64_t max_video_bytes(bool over_modem)
{
#if CONFIG_CAMERA_DOWNLOAD_VIDEOS
#if CONFIG_MODEM_UPLINK
    if (over_modem) {
        return (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB_MODEM * 1024 * 1024;
    }
#endif
    (void)over_modem;
    return (std::uint64_t)CONFIG_UPLINK_VIDEO_MAX_MB * 1024 * 1024;
#else
    (void)over_modem;
    return 50ull * 1024 * 1024;
#endif
}

}  // namespace uplink
