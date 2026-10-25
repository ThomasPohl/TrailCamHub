#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "camera_wifi.h"

#define CAMERA_HTTP_HOST "192.168.1.8"
#define CAMERA_HTTP_PORT 80
#define CAMERA_HTTP_TIMEOUT_MS 10000
#define CAMERA_HTTP_RESPONSE_MAX_LEN 1024
#define WIFI_CONNECT_MAX_RETRIES 5
#define WIFI_GOT_IP_BIT BIT0

typedef struct {
    char body[CAMERA_HTTP_RESPONSE_MAX_LEN];
    size_t body_len;
    bool too_large;
} http_response_t;

static const char *TAG = "trailcamhub_wifi";
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_connect_requested;
static unsigned s_wifi_connect_retries;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t data_len = (size_t)event->data_len;
        if (data_len >= sizeof(response->body) - response->body_len) {
            response->too_large = true;
            return ESP_FAIL;
        }
        memcpy(response->body + response->body_len, event->data, data_len);
        response->body_len += data_len;
    }

    return ESP_OK;
}

static esp_err_t camera_http_get(const char *path, http_response_t *response)
{
    char url[128];
    int url_len = snprintf(url, sizeof(url), "http://%s:%d%s",
                           CAMERA_HTTP_HOST, CAMERA_HTTP_PORT, path);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        ESP_LOGE(TAG, "Camera HTTP URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CAMERA_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Could not initialize camera HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        if (response->too_large) {
            ESP_LOGE(TAG, "Camera response from %s exceeded %u bytes", path,
                     (unsigned)(sizeof(response->body) - 1));
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_LOGE(TAG, "Camera HTTP request %s failed: %s", path,
                 esp_err_to_name(error));
        return error;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Camera HTTP request %s returned status %d", path,
                 status_code);
        return ESP_FAIL;
    }

    response->body[response->body_len] = '\0';
    ESP_LOGI(TAG, "Camera HTTP GET %s returned status %d", path, status_code);
    return ESP_OK;
}

static void fetch_camera_storage_summary(void)
{
    http_response_t response;

    ESP_LOGI(TAG, "Switching camera to storage mode");
    esp_err_t error = camera_http_get("/SetMode?Storage", &response);
    if (error == ESP_OK) {
        error = camera_http_get("/Storage?GetDirFileInfo", &response);
    }
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Camera storage summary: %s", response.body);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        if (!s_wifi_connect_requested) {
            return;
        }
        if (s_wifi_connect_retries >= WIFI_CONNECT_MAX_RETRIES) {
            ESP_LOGE(TAG, "Camera Wi-Fi connection failed after %u retries "
                     "(reason %d)", WIFI_CONNECT_MAX_RETRIES, event->reason);
            s_wifi_connect_requested = false;
            return;
        }

        s_wifi_connect_retries++;
        ESP_LOGW(TAG, "Camera Wi-Fi disconnected (reason %d); retry %u/%u",
                 event->reason, s_wifi_connect_retries,
                 WIFI_CONNECT_MAX_RETRIES);
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Camera Wi-Fi reconnect failed: %s",
                     esp_err_to_name(error));
            s_wifi_connect_requested = false;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to camera Wi-Fi; IP address: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_wifi_connect_retries = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

static void camera_wifi_worker(void *arg)
{
    (void)arg;
    while (true) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT, pdTRUE,
                            pdFALSE, portMAX_DELAY);
        fetch_camera_storage_summary();
    }
}

esp_err_t camera_wifi_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG,
                        "Could not initialize network interface");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "Could not create default event loop");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi station interface");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG,
                        "Could not initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Could not restrict Wi-Fi credentials to RAM");

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "Could not register IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Could not select station mode");

    BaseType_t task_created = xTaskCreate(camera_wifi_worker, "camera_wifi",
                                          6144, NULL, 5, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Could not create camera Wi-Fi worker task");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Could not start Wi-Fi");
    ESP_LOGI(TAG, "Camera Wi-Fi station is ready");
    return ESP_OK;
}

esp_err_t camera_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    wifi_config_t wifi_config = {0};
    if (ssid_len == 0 || ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "Camera Wi-Fi credentials exceed supported field lengths");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    s_wifi_connect_retries = 0;
    s_wifi_connect_requested = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "Could not set camera Wi-Fi credentials");
    s_wifi_connect_requested = true;
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        s_wifi_connect_requested = false;
        ESP_LOGE(TAG, "Could not start camera Wi-Fi connection: %s",
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "Connecting to camera Wi-Fi network \"%s\"", ssid);
    return ESP_OK;
}
