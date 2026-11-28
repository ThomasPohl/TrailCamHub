#pragma once
// The HTTP side of the whole-cycle test (#446): every request the firmware
// makes through esp_http_client goes to the handler set here, in process.
// Phase 2 sets a handler that forwards to the local backend.
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace host_http
{

struct Request {
    std::string method;  // "GET", "POST", ...
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string user;  // HTTP Basic, "" if none
    std::string password;
    std::string body;

    // The value of a header (case-insensitive name), "" if not sent.
    std::string header(const std::string &name) const;
};

struct Response {
    int status = 200;
    std::string body;
    // Without a Content-Length, like a chunked response.
    bool no_length = false;
};

// No handler: every request fails to connect (no network).
using Handler = std::function<Response(const Request &)>;
void set_handler(Handler handler);

// The requests sent since the last clear_requests().
const std::vector<Request> &requests();
void clear_requests();

}  // namespace host_http
