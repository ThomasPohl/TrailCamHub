#pragma once

// HTTP(S) helpers shared by the destinations (backend, WebDAV, Telegram).
// Server certificates are checked against the ESP-IDF certificate bundle;
// http:// URLs (a NAS in the home network) are sent unencrypted.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "esp_err.h"
#include "esp_http_client.h"

namespace uplink
{

struct HttpResult {
    esp_err_t error = ESP_FAIL;
    int status = 0;
    std::string body;
};

// A client for one request; nullptr if out of memory.
esp_http_client_handle_t open_client(const std::string &url,
                                     esp_http_client_method_t method);

// Reads the status and up to 64 KB of body.
HttpResult read_response(esp_http_client_handle_t client);

// "HTTP 404 <start of body>" or the transport error.
std::string describe(const HttpResult &result);

// Fills `buffer` with up to `size` bytes of the body at `offset`; returns the
// number of bytes, or a negative value on an error.
using BodyReader = std::function<std::int64_t(
    std::uint64_t offset, std::uint8_t *buffer, std::size_t size)>;

struct Request {
    std::string url;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    std::vector<std::pair<std::string, std::string>> headers;
    // HTTP Basic authentication if `user` is not empty.
    std::string user;
    std::string password;
    std::uint64_t body_size = 0;
    BodyReader body;  // may be empty with body_size 0
};

// Sends a request, streaming its body in 16 KB pieces; `read_us` adds the
// time spent in the body reader (the SD card).
HttpResult send(const Request &request, std::int64_t *read_us = nullptr);

// A body from a string.
BodyReader string_body(const std::string &text);

// A body from a file on the SD card; ESP_ERR_NOT_FOUND if it cannot be
// opened. The file stays open until the reader is destroyed.
esp_err_t file_body(const std::string &path, BodyReader &reader,
                    std::uint64_t &size);

// The largest video the build uploads over this transport.
std::uint64_t max_video_bytes(bool over_modem);

}  // namespace uplink
