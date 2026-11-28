#pragma once
// Host: esp_http_client hands each request to the test's HTTP handler
// (host_http.hpp, #446) instead of the network.
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_HTTP_BASE 0x7000
#define ESP_ERR_HTTP_FETCH_HEADER (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_CONNECT (ESP_ERR_HTTP_BASE + 3)

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
} esp_http_client_method_t;

typedef enum {
    HTTP_AUTH_TYPE_NONE,
    HTTP_AUTH_TYPE_BASIC,
} esp_http_client_auth_type_t;

struct HostHttpClient;
typedef HostHttpClient *esp_http_client_handle_t;

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int buffer_size;
    int buffer_size_tx;
    bool keep_alive_enable;
} esp_http_client_config_t;

extern "C" {
esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value);
esp_err_t esp_http_client_set_authtype(esp_http_client_handle_t client,
                                       esp_http_client_auth_type_t type);
esp_err_t esp_http_client_set_username(esp_http_client_handle_t client,
                                       const char *username);
esp_err_t esp_http_client_set_password(esp_http_client_handle_t client,
                                       const char *password);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer,
                          int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer,
                         int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
}
