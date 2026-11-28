#include "host_http.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

struct HostHttpClient {
    host_http::Request request;
    host_http::Response response;
    int expected_body = 0;
    bool opened = false;
    bool fetched = false;
    std::size_t read_position = 0;
};

namespace
{

host_http::Handler s_handler;
std::vector<host_http::Request> s_requests;

const char *method_name(esp_http_client_method_t method)
{
    switch (method) {
    case HTTP_METHOD_GET:
        return "GET";
    case HTTP_METHOD_POST:
        return "POST";
    case HTTP_METHOD_PUT:
        return "PUT";
    case HTTP_METHOD_PATCH:
        return "PATCH";
    case HTTP_METHOD_DELETE:
        return "DELETE";
    case HTTP_METHOD_HEAD:
        return "HEAD";
    }
    return "GET";
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

}  // namespace

namespace host_http
{

std::string Request::header(const std::string &name) const
{
    for (const auto &entry : headers) {
        if (lower(entry.first) == lower(name)) {
            return entry.second;
        }
    }
    return "";
}

void set_handler(Handler handler)
{
    s_handler = std::move(handler);
}

const std::vector<Request> &requests()
{
    return s_requests;
}

void clear_requests()
{
    s_requests.clear();
}

}  // namespace host_http

extern "C" {

esp_err_t esp_crt_bundle_attach(void *)
{
    return ESP_OK;
}

esp_log_level_t esp_log_level_get(const char *)
{
    return ESP_LOG_INFO;
}

void esp_log_level_set(const char *, esp_log_level_t)
{
}

esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *config)
{
    auto *client = new HostHttpClient;
    client->request.url = config->url != nullptr ? config->url : "";
    client->request.method = method_name(config->method);
    return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value)
{
    client->request.headers.emplace_back(key, value);
    return ESP_OK;
}

esp_err_t esp_http_client_set_authtype(esp_http_client_handle_t,
                                       esp_http_client_auth_type_t)
{
    return ESP_OK;
}

esp_err_t esp_http_client_set_username(esp_http_client_handle_t client,
                                       const char *username)
{
    client->request.user = username;
    return ESP_OK;
}

esp_err_t esp_http_client_set_password(esp_http_client_handle_t client,
                                       const char *password)
{
    client->request.password = password;
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    if (!s_handler) {
        return ESP_ERR_HTTP_CONNECT;
    }
    client->opened = true;
    client->expected_body = write_len;
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t client, const char *buffer,
                          int len)
{
    if (!client->opened || len < 0 ||
        (int)client->request.body.size() + len > client->expected_body) {
        return -1;
    }
    client->request.body.append(buffer, (std::size_t)len);
    return len;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (!client->opened || !s_handler ||
        (int)client->request.body.size() != client->expected_body) {
        return -1;
    }
    s_requests.push_back(client->request);
    client->response = s_handler(client->request);
    client->fetched = true;
    return client->response.no_length ? 0
                                      : (int64_t)client->response.body.size();
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client->fetched ? client->response.status : 0;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{
    if (!client->fetched || client->response.no_length) {
        return -1;
    }
    return (int64_t)client->response.body.size();
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    if (!client->fetched || len < 0) {
        return -1;
    }
    const std::string &body = client->response.body;
    const std::size_t n =
        std::min((std::size_t)len, body.size() - client->read_position);
    std::memcpy(buffer, body.data() + client->read_position, n);
    client->read_position += n;
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    client->opened = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    delete client;
    return ESP_OK;
}

}  // extern "C"
