/*
 * This module serves the web UI and its JSON API (see web_ui.h) over the
 * dongle's own access point, or, with CONFIG_WEB_UI_OVER_ETHERNET, on the
 * Ethernet interface at the address that DHCP assigned:
 *   GET  /               the page (index.html, style.css, app.js); also at
 *                        /browse/<path>, the address of a directory or file
 *                        on the SD card, so that the page can be bookmarked
 *   GET  /api/status     workflow status, SD card, memory and, for each camera
 *                        on the card, the time of its last sync as JSON
 *   POST /api/search     search for the camera now
 *   POST /api/stop       stop searching or end the session
 *   GET  /api/files      directory listing (?path=a/b) as JSON
 *   GET  /api/log        bytes of the current log file from ?offset=N
 *   GET  /files/<path>   a file from the SD card (?download=1: as attachment);
 *                        <name>.mp4 is built from <name>.h264 and <name>.aac
 *                        when there is no such file (CONFIG_CAMERA_VIDEO_MP4_
 *                        ON_REQUEST); Range requests are served for it
 *   POST /api/delete     delete a file (?path=a/b)
 *   POST /api/time       set the clock from the browser (?unix=<UTC seconds>);
 *                        only if no better source has set it
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <memory>
#include <string>
#include <vector>

#if !CONFIG_WEB_UI_OVER_ETHERNET
#include "dhcpserver/dhcpserver.h"
#endif
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#if !CONFIG_WEB_UI_OVER_ETHERNET
#include "esp_mac.h"
#endif
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#if !CONFIG_WEB_UI_OVER_ETHERNET
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_CAMERA_VIDEO_MP4_ON_REQUEST
#include "lwip/sockets.h"
#include "mp4_mux.hpp"
#endif
#include "sd_card.h"
#include "sd_log.h"
#include "time_sync.h"
#include "web_paths.hpp"
#include "web_ui.h"

// Until the camera channel is known.
#define WEB_AP_DEFAULT_CHANNEL 1
#define WEB_AP_MAX_CLIENTS 3
#define WEB_PASSWORD_LENGTH 10
#define WEB_FILE_CHUNK 4096
#define WEB_LOG_MAX_READ (16 * 1024)
#define WEB_MAX_LIST_ENTRIES 2000
#define WEB_SD_INFO_INTERVAL_US (10 * 1000 * 1000)
#define WEB_NVS_NAMESPACE "web_ui"
#define WEB_NVS_PASSWORD_KEY "ap_key"
#define WEB_NVS_CHANNEL_KEY "ap_channel"
// Largest free block of internal RAM needed to open the access point again.
// When the Wi-Fi driver cannot allocate the beacon it crashes, so the access
// point stays closed until the next status change finds enough memory.
#define WEB_AP_REOPEN_MIN_BLOCK (8 * 1024)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[] asm("_binary_style_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

static const char *TAG = "trailcamhub_web";

namespace
{

web_ui_callbacks_t s_callbacks;
httpd_handle_t s_server;
char s_ssid[33];
char s_password[65];
char s_url[32];
#if !CONFIG_WEB_UI_OVER_ETHERNET
// Kept to open the access point again after web_ui_suspend_access_point().
wifi_config_t s_ap_config;
bool s_ap_started;
// Closed by web_ui_suspend_access_point(), or still closed because the last
// attempt to open it found too little memory. Shared under s_lock.
bool s_ap_suspended;
bool s_ap_reopen_wanted;
#endif

SemaphoreHandle_t s_lock;
// Shared with the setters under s_lock.
char s_phase[16] = "idle";
char s_camera_ssid[33];
int64_t s_retry_deadline_us;
uint32_t s_media_done;
uint32_t s_media_stored;
uint32_t s_media_total;

// Cached because computing the free space of a large card is slow.
int64_t s_sd_info_us;
uint64_t s_sd_total;
uint64_t s_sd_free;

#if CONFIG_WEB_UI_OVER_ETHERNET
// The page is reachable as soon as the Ethernet interface has an address from
// DHCP; the address is kept for web_ui_url() and logged.
void set_url(const char *url)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_url, sizeof(s_url), "%s", url);
    xSemaphoreGive(s_lock);
}

void on_ethernet_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    char url[sizeof(s_url)];
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event =
            static_cast<const ip_event_got_ip_t *>(data);
        snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&event->ip_info.ip));
        set_url(url);
        ESP_LOGI(TAG, "Web UI at %s", url);
    } else {
        set_url("");
        ESP_LOGI(TAG, "Web UI: Ethernet address lost");
    }
}

esp_err_t start_network()
{
    snprintf(s_ssid, sizeof(s_ssid), "Ethernet");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_GOT_IP,
                                                   on_ethernet_ip, NULL),
                        TAG, "IP event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_LOST_IP,
                                                   on_ethernet_ip, NULL),
                        TAG, "IP event handler");
    // The address may have been assigned before this module started.
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
        ip.ip.addr != 0) {
        char url[sizeof(s_url)];
        snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&ip.ip));
        set_url(url);
        ESP_LOGI(TAG, "Web UI at %s", url);
    } else {
        ESP_LOGI(TAG, "Web UI waits for an Ethernet address");
    }
    return ESP_OK;
}
#else
// Characters of generated passwords. Left out are those that look alike in
// the display's 5x7 font or elsewhere: g, q and 9, i, l and 1, o and 0.
const char kPasswordAlphabet[] = "abcdefhjkmnprstuvwxyz2345678";

bool generated_password_valid(const char *password)
{
    if (strlen(password) != WEB_PASSWORD_LENGTH) {
        return false;
    }
    for (const char *c = password; *c != '\0'; c++) {
        if (strchr(kPasswordAlphabet, *c) == NULL) {
            return false;
        }
    }
    return true;
}

// Loads the access point password from NVS, or generates and stores one. A
// stored password from an older alphabet is replaced, because it may not be
// readable on the display.
void load_password()
{
    if (strlen(CONFIG_WEB_UI_AP_PASSWORD) >= 8) {
        snprintf(s_password, sizeof(s_password), "%s",
                 CONFIG_WEB_UI_AP_PASSWORD);
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(WEB_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        size_t size = sizeof(s_password);
        if (nvs_get_str(nvs, WEB_NVS_PASSWORD_KEY, s_password, &size) ==
                ESP_OK &&
            generated_password_valid(s_password)) {
            nvs_close(nvs);
            return;
        }
    } else {
        nvs = 0;
    }
    for (int i = 0; i < WEB_PASSWORD_LENGTH; i++) {
        s_password[i] =
            kPasswordAlphabet[esp_random() % (sizeof(kPasswordAlphabet) - 1)];
    }
    s_password[WEB_PASSWORD_LENGTH] = '\0';
    ESP_LOGI(TAG, "Generated a new access point password; it is shown on "
                  "the display");
    if (nvs != 0) {
        if (nvs_set_str(nvs, WEB_NVS_PASSWORD_KEY, s_password) != ESP_OK ||
            nvs_commit(nvs) != ESP_OK) {
            ESP_LOGW(TAG, "Could not store the access point password");
        }
        nvs_close(nvs);
    }
}

// The access point has to use the channel of the joined camera. Starting it
// on the channel of the last camera spares the clients the switch.
uint8_t load_channel()
{
    uint8_t channel = WEB_AP_DEFAULT_CHANNEL;
    nvs_handle_t nvs;
    if (nvs_open(WEB_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, WEB_NVS_CHANNEL_KEY, &channel);
        nvs_close(nvs);
    }
    return channel;
}

void on_station_connected(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const uint8_t channel =
        static_cast<const wifi_event_sta_connected_t *>(data)->channel;
    if (channel == 0 || channel == load_channel()) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(WEB_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, WEB_NVS_CHANNEL_KEY, channel) == ESP_OK &&
            nvs_commit(nvs) == ESP_OK) {
            ESP_LOGI(TAG,
                     "Access point follows the camera to channel %u; "
                     "it starts there from the next boot",
                     channel);
        }
        nvs_close(nvs);
    }
}

esp_err_t start_access_point()
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "AP interface");

    // A subnet that camera access points are unlikely to use, and no router
    // in the DHCP offer, so phones keep their mobile data for the internet.
    esp_netif_ip_info_t ip = {};
    esp_netif_set_ip4_addr(&ip.ip, 10, 42, 0, 1);
    esp_netif_set_ip4_addr(&ip.gw, 10, 42, 0, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "AP address");
    dhcps_offer_t offer_router = 0;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer_router,
                           sizeof(offer_router));
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(netif), TAG, "DHCP server");
    snprintf(s_url, sizeof(s_url), "http://" IPSTR, IP2STR(&ip.ip));

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "TrailCamHub-%02X%02X", mac[4], mac[5]);
    load_password();

    wifi_config_t config = {};
    memcpy(config.ap.ssid, s_ssid, strlen(s_ssid));
    config.ap.ssid_len = strlen(s_ssid);
    memcpy(config.ap.password, s_password, strlen(s_password));
    config.ap.channel = load_channel();
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = WEB_AP_MAX_CLIENTS;
    config.ap.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "AP+STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG,
                        "AP configuration");
    s_ap_config = config;
    s_ap_started = true;
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   WIFI_EVENT_STA_CONNECTED,
                                                   on_station_connected, NULL),
                        TAG, "station event handler");
    ESP_LOGI(TAG, "Access point \"%s\" open on channel %u; web UI at %s",
             s_ssid, config.ap.channel, s_url);
    return ESP_OK;
}

esp_err_t start_network()
{
    return start_access_point();
}
#endif

// Path below the SD card root from a request parameter; false if unsafe.
bool query_path(httpd_req_t *req, const char *key, std::string &relative)
{
    relative.clear();
    const size_t length = httpd_req_get_url_query_len(req);
    if (length == 0) {
        return true;
    }
    std::string query(length + 1, '\0');
    if (httpd_req_get_url_query_str(req, &query[0], query.size()) != ESP_OK) {
        return true;
    }
    char raw[256];
    if (httpd_query_key_value(query.c_str(), key, raw, sizeof(raw)) != ESP_OK) {
        return true;
    }
    std::string decoded;
    return web::url_decode(raw, decoded) &&
           web::sanitize_path(decoded, relative);
}

// A number from the query string. 64 bits: a long has 32 on the ESP32, and a
// Unix time beyond 2038 is cut off at its largest value.
long long query_int64(httpd_req_t *req, const char *key, long long fallback)
{
    const size_t length = httpd_req_get_url_query_len(req);
    if (length == 0) {
        return fallback;
    }
    std::string query(length + 1, '\0');
    char value[24];
    if (httpd_req_get_url_query_str(req, &query[0], query.size()) != ESP_OK ||
        httpd_query_key_value(query.c_str(), key, value, sizeof(value)) !=
            ESP_OK) {
        return fallback;
    }
    return strtoll(value, NULL, 10);
}

long query_long(httpd_req_t *req, const char *key, long fallback)
{
    return static_cast<long>(query_int64(req, key, fallback));
}

std::string absolute(const std::string &relative)
{
    return relative.empty() ? std::string(SD_CARD_MOUNT_POINT)
                            : std::string(SD_CARD_MOUNT_POINT) + "/" + relative;
}

esp_err_t send_json(httpd_req_t *req, const std::string &json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), json.size());
}

// A file of the page, embedded in the firmware (with a trailing NUL).
struct Asset {
    const char *start;
    const char *end;
    const char *type;
};

const Asset kIndexHtml = {index_html_start, index_html_end,
                          "text/html; charset=utf-8"};
const Asset kStyleCss = {style_css_start, style_css_end,
                         "text/css; charset=utf-8"};
const Asset kAppJs = {app_js_start, app_js_end,
                      "text/javascript; charset=utf-8"};

esp_err_t handle_asset(httpd_req_t *req)
{
    const Asset *asset = static_cast<const Asset *>(req->user_ctx);
    httpd_resp_set_type(req, asset->type);
    // Revalidate, so a new firmware is picked up at once.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, asset->start, asset->end - asset->start - 1);
}

// The cameras on the SD card (a camera.json in a directory of the root) and
// when each was last synced; read every few seconds, as the page asks often.
struct CameraEntry {
    std::string dir;
    std::string id;
    long long last_sync =
        0;  // Unix seconds; -1: synced, time unknown; 0: never
    long long read_at = 0;
};
std::vector<CameraEntry> s_cameras;
int64_t s_cameras_us;

void refresh_cameras(int64_t now_us)
{
    if (s_cameras_us != 0 && now_us - s_cameras_us < WEB_SD_INFO_INTERVAL_US) {
        return;
    }
    s_cameras_us = now_us;
    s_cameras.clear();
    DIR *root = opendir(SD_CARD_MOUNT_POINT);
    if (root == NULL) {
        return;
    }
    while (struct dirent *entry = readdir(root)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = std::string(SD_CARD_MOUNT_POINT) + "/" +
                                 entry->d_name + "/camera.json";
        FILE *file = fopen(path.c_str(), "rb");
        if (file == NULL) {
            continue;
        }
        char text[512];
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        const std::string json(text, length);
        CameraEntry camera;
        camera.dir = entry->d_name;
        web::json_string(json, "devId", camera.id);
        web::json_int(json, "lastSyncAt", camera.last_sync);
        web::json_int(json, "readAt", camera.read_at);
        s_cameras.push_back(camera);
        if (s_cameras.size() >= 8) {
            break;
        }
    }
    closedir(root);
}

esp_err_t handle_status(httpd_req_t *req)
{
    char phase[sizeof(s_phase)];
    char camera_ssid[sizeof(s_camera_ssid)];
    int64_t deadline_us;
    uint32_t done;
    uint32_t stored;
    uint32_t total;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(phase, s_phase, sizeof(phase));
    memcpy(camera_ssid, s_camera_ssid, sizeof(camera_ssid));
    deadline_us = s_retry_deadline_us;
    done = s_media_done;
    stored = s_media_stored;
    total = s_media_total;
    xSemaphoreGive(s_lock);

    const int64_t now_us = esp_timer_get_time();
    if (s_sd_info_us == 0 || now_us - s_sd_info_us > WEB_SD_INFO_INTERVAL_US) {
        if (esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &s_sd_total, &s_sd_free) !=
            ESP_OK) {
            s_sd_total = 0;
            s_sd_free = 0;
        }
        s_sd_info_us = now_us;
    }
    const int64_t retry_ms =
        deadline_us > now_us ? (deadline_us - now_us) / 1000 : 0;
    char log_path[64];
    sd_log_current_path(log_path, sizeof(log_path));
    const char *log_relative = log_path;
    if (strncmp(log_path, SD_CARD_MOUNT_POINT "/",
                strlen(SD_CARD_MOUNT_POINT) + 1) == 0) {
        log_relative += strlen(SD_CARD_MOUNT_POINT) + 1;
    }

    std::string json = "{\"phase\":";
    web::append_json_string(json, phase);
    json += ",\"camera\":";
    web::append_json_string(json, camera_ssid);
    char numbers[320];
    snprintf(numbers, sizeof(numbers),
             ",\"retry_ms\":%lld,\"media_done\":%lu,\"media_stored\":%lu,"
             "\"media_total\":%lu,"
             "\"sd_total_mb\":%llu,\"sd_free_mb\":%llu,\"uptime_s\":%lld,"
             "\"heap_internal\":%u,\"heap_psram\":%u,\"log\":",
             (long long)retry_ms, (unsigned long)done, (unsigned long)stored,
             (unsigned long)total,
             (unsigned long long)(s_sd_total / (1024 * 1024)),
             (unsigned long long)(s_sd_free / (1024 * 1024)),
             (long long)(now_us / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    json += numbers;
    web::append_json_string(json, log_relative);
    json += ",\"ap\":";
    web::append_json_string(json, s_ssid);
    // A clock before 2024 has not been set: times cannot be recorded.
    const bool clock_valid = time_sync_clock_valid();
    json += clock_valid ? ",\"clock_set\":true" : ",\"clock_set\":false";
    json += ",\"clock_source\":";
    web::append_json_string(
        json,
        clock_valid ? timepolicy::source_name(time_sync_source()) : "none");
    char now_text[40];
    snprintf(now_text, sizeof(now_text), ",\"time\":%lld",
             clock_valid ? (long long)time(NULL) : 0LL);
    json += now_text;
    refresh_cameras(now_us);
    json += ",\"cameras\":[";
    for (size_t i = 0; i < s_cameras.size(); i++) {
        if (i > 0) {
            json += ",";
        }
        json += "{\"dir\":";
        web::append_json_string(json, s_cameras[i].dir);
        json += ",\"id\":";
        web::append_json_string(json, s_cameras[i].id);
        char times[72];
        snprintf(times, sizeof(times), ",\"last_sync\":%lld,\"read_at\":%lld}",
                 s_cameras[i].last_sync, s_cameras[i].read_at);
        json += times;
    }
    json += "]}";
    return send_json(req, json);
}

// The browser tells the time of its computer. It is taken only if no better
// source (SNTP, the backend) has set the clock, and it replaces the camera's.
esp_err_t handle_time(httpd_req_t *req)
{
    const long long seconds = query_int64(req, "unix", 0);
    if (seconds <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no time");
    }
    const bool set = time_sync_set(seconds, timepolicy::Source::browser);
    std::string json = std::string("{\"ok\":true,\"set\":") +
                       (set ? "true" : "false") + ",\"source\":";
    web::append_json_string(json,
                            time_sync_clock_valid()
                                ? timepolicy::source_name(time_sync_source())
                                : "none");
    json += "}";
    return send_json(req, json);
}

esp_err_t handle_search(httpd_req_t *req)
{
    if (s_callbacks.on_search != NULL) {
        s_callbacks.on_search();
    }
    return send_json(req, "{\"ok\":true}");
}

esp_err_t handle_stop(httpd_req_t *req)
{
    if (s_callbacks.on_stop != NULL) {
        s_callbacks.on_stop();
    }
    return send_json(req, "{\"ok\":true}");
}

#if CONFIG_CAMERA_VIDEO_MP4_ON_REQUEST
std::string offered_mp4(const std::string &directory, const std::string &name);
#endif

esp_err_t handle_files(httpd_req_t *req)
{
    std::string relative;
    if (!query_path(req, "path", relative)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string directory = absolute(relative);
    DIR *dir = opendir(directory.c_str());
    if (dir == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no directory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    std::string chunk = "{\"path\":";
    web::append_json_string(chunk, relative);
    chunk += ",\"entries\":[";
    bool first = true;
    int count = 0;
    while (struct dirent *entry = readdir(dir)) {
        if (++count > WEB_MAX_LIST_ENTRIES) {
            break;
        }
        const std::string full = directory + "/" + entry->d_name;
        struct stat info = {};
        stat(full.c_str(), &info);
        if (!first) {
            chunk += ",";
        }
        first = false;
        chunk += "{\"name\":";
        web::append_json_string(chunk, entry->d_name);
        char fields[96];
        snprintf(fields, sizeof(fields),
                 ",\"dir\":%s,\"size\":%lld,\"mtime\":%lld}",
                 S_ISDIR(info.st_mode) ? "true" : "false",
                 (long long)info.st_size, (long long)info.st_mtime);
        chunk += fields;
#if CONFIG_CAMERA_VIDEO_MP4_ON_REQUEST
        // The MP4 of a video is built when it is requested; the size listed
        // is that of its two streams, close to the one that is sent.
        const std::string mp4_name = offered_mp4(directory, entry->d_name);
        if (!mp4_name.empty()) {
            long long size = (long long)info.st_size;
            struct stat audio = {};
            const std::string base =
                directory + "/" +
                std::string(entry->d_name, strlen(entry->d_name) - 5);
            if (stat((base + ".aac").c_str(), &audio) == 0) {
                size += (long long)audio.st_size;
            }
            chunk += ",{\"name\":";
            web::append_json_string(chunk, mp4_name);
            snprintf(fields, sizeof(fields),
                     ",\"dir\":false,\"size\":%lld,\"mtime\":%lld}", size,
                     (long long)info.st_mtime);
            chunk += fields;
        }
#endif
        if (chunk.size() > 1024) {
            if (httpd_resp_send_chunk(req, chunk.data(), chunk.size()) !=
                ESP_OK) {
                closedir(dir);
                return ESP_FAIL;
            }
            chunk.clear();
        }
    }
    closedir(dir);
    chunk += "]}";
    httpd_resp_send_chunk(req, chunk.data(), chunk.size());
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Sends `size` bytes of `file` from its current position in chunks.
esp_err_t send_file_bytes(httpd_req_t *req, FILE *file, long size)
{
    char *buffer = static_cast<char *>(
        heap_caps_malloc(WEB_FILE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == NULL) {
        buffer = static_cast<char *>(malloc(WEB_FILE_CHUNK));
    }
    if (buffer == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    esp_err_t error = ESP_OK;
    while (size != 0) {
        const size_t want =
            size < 0 || size > WEB_FILE_CHUNK ? WEB_FILE_CHUNK : (size_t)size;
        const size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            break;
        }
        error = httpd_resp_send_chunk(req, buffer, got);
        if (error != ESP_OK) {
            break;
        }
        if (size > 0) {
            size -= got;
        }
    }
    free(buffer);
    if (error != ESP_OK) {
        // The client went away; close the connection.
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

#if CONFIG_CAMERA_VIDEO_MP4_ON_REQUEST
#define WEB_MP4_CHUNK (16 * 1024)

// A stream file for the MP4 builder. It opens the file when it is read and
// remembers the position, so the sequential scans do not seek. release()
// closes the file: the builder is kept between requests, but no file stays
// open while a camera session writes to the card.
class StreamSource : public mp4::Source
{
  public:
    explicit StreamSource(const std::string &path) : m_path(path)
    {
        struct stat info = {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            m_size = (uint64_t)info.st_size;
            m_mtime = (long long)info.st_mtime;
            m_exists = true;
        }
    }
    ~StreamSource() override
    {
        release();
    }
    bool exists() const
    {
        return m_exists;
    }
    long long mtime() const
    {
        return m_mtime;
    }
    uint64_t size() const override
    {
        return m_size;
    }
    // Time spent in reads since the last call, in ms.
    int64_t take_read_ms()
    {
        const int64_t ms = m_read_us / 1000;
        m_read_us = 0;
        return ms;
    }
    void release()
    {
        if (m_file != NULL) {
            fclose(m_file);
            m_file = NULL;
        }
        m_position = 0;
    }
    bool read(uint64_t offset, uint8_t *destination, size_t length) override
    {
        if (!m_exists || offset + length > m_size) {
            return false;
        }
        if (m_file == NULL) {
            m_file = fopen(m_path.c_str(), "rb");
            m_position = 0;
            if (m_file == NULL) {
                return false;
            }
        }
        const int64_t started_us = esp_timer_get_time();
        bool ok = true;
        if (offset != m_position &&
            fseek(m_file, (long)offset, SEEK_SET) != 0) {
            ok = false;
        }
        if (ok && fread(destination, 1, length, m_file) != length) {
            ok = false;
        }
        m_position = ok ? offset + length : UINT64_MAX;
        m_read_us += esp_timer_get_time() - started_us;
        return ok;
    }

  private:
    std::string m_path;
    FILE *m_file = NULL;
    bool m_exists = false;
    uint64_t m_size = 0;
    uint64_t m_position = 0;
    long long m_mtime = 0;
    int64_t m_read_us = 0;
};

// The MP4 that was built last, kept so that the next request for it (a
// player asks several times, to seek) does not scan the video again. Only
// the HTTP server task uses it.
struct Mp4Cache {
    std::string path;
    std::unique_ptr<StreamSource> video;
    std::unique_ptr<StreamSource> audio;
    std::unique_ptr<mp4::Mp4File> file;

    void reset()
    {
        file.reset();
        video.reset();
        audio.reset();
        path.clear();
    }
};
Mp4Cache s_mp4_cache;

// The camera session writes the SD card at the limit of what it can do, so
// an MP4 is not built while one runs.
bool session_active()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active =
        strcmp(s_phase, "idle") != 0 && strcmp(s_phase, "finished") != 0;
    xSemaphoreGive(s_lock);
    return active;
}

bool send_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        const int sent = send(fd, data, length, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

// Sends <name>.mp4, built from the two stream files of the video. The
// response is written to the socket directly: the HTTP server's chunked
// responses carry no Content-Length, which players need for seeking.
esp_err_t serve_mp4(httpd_req_t *req, const std::string &relative,
                    bool download)
{
    std::string video_relative;
    std::string audio_relative;
    if (!web::mp4_sources(relative, video_relative, audio_relative)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
    }
    struct stat info = {};
    if (stat(absolute(video_relative).c_str(), &info) != 0 ||
        S_ISDIR(info.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
    }
    if (session_active()) {
        // Do not hold the builder, or files, while the camera session runs.
        s_mp4_cache.reset();
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "20");
        return httpd_resp_sendstr(
            req, "A camera session is running; try again when it has ended.");
    }

    // The files are closed again whichever way this request ends.
    struct ReleaseFiles {
        ~ReleaseFiles()
        {
            if (s_mp4_cache.video != nullptr) {
                s_mp4_cache.video->release();
            }
            if (s_mp4_cache.audio != nullptr) {
                s_mp4_cache.audio->release();
            }
        }
    } release_files;

    // The cached builder is good if the streams are the ones it scanned.
    std::unique_ptr<StreamSource> video(
        new (std::nothrow) StreamSource(absolute(video_relative)));
    std::unique_ptr<StreamSource> audio(
        new (std::nothrow) StreamSource(absolute(audio_relative)));
    if (video == nullptr || audio == nullptr || !video->exists()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    const bool cached = s_mp4_cache.file != nullptr &&
                        s_mp4_cache.path == relative &&
                        s_mp4_cache.video->size() == video->size() &&
                        s_mp4_cache.video->mtime() == video->mtime() &&
                        s_mp4_cache.audio->exists() == audio->exists() &&
                        s_mp4_cache.audio->size() == audio->size() &&
                        s_mp4_cache.audio->mtime() == audio->mtime();
    if (!cached) {
        s_mp4_cache.reset();
        std::unique_ptr<mp4::Mp4File> file(new (std::nothrow) mp4::Mp4File);
        if (file == nullptr) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "no memory");
        }
        const int64_t started_us = esp_timer_get_time();
        if (!file->open(*video, audio->exists() ? audio.get() : nullptr)) {
            ESP_LOGW(TAG, "Cannot build %s: %s", relative.c_str(),
                     file->error().c_str());
            video->release();
            audio->release();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       file->error().c_str());
        }
        ESP_LOGI(TAG,
                 "%s: %u video and %u audio frames, %llu bytes, scanned in "
                 "%lld ms (%lld ms of it reading)",
                 relative.c_str(), (unsigned)file->video_frames(),
                 (unsigned)file->audio_frames(),
                 (unsigned long long)file->size(),
                 (long long)((esp_timer_get_time() - started_us) / 1000),
                 (long long)(video->take_read_ms() + audio->take_read_ms()));
        s_mp4_cache.path = relative;
        s_mp4_cache.video = std::move(video);
        s_mp4_cache.audio = std::move(audio);
        s_mp4_cache.file = std::move(file);
    }
    mp4::Mp4File *const file = s_mp4_cache.file.get();
    const uint64_t size = file->size();

    uint64_t first = 0;
    uint64_t last = size - 1;
    const char *status = "200 OK";
    web::RangeResult range = web::RangeResult::kNone;
    const size_t range_length = httpd_req_get_hdr_value_len(req, "Range");
    if (range_length > 0 && range_length < 80) {
        char value[80];
        if (httpd_req_get_hdr_value_str(req, "Range", value, sizeof(value)) ==
            ESP_OK) {
            range = web::parse_range(value, size, first, last);
        }
    }
    char extra[96] = "";
    if (range == web::RangeResult::kPartial) {
        status = "206 Partial Content";
        snprintf(extra, sizeof(extra),
                 "Content-Range: bytes %llu-%llu/%llu\r\n",
                 (unsigned long long)first, (unsigned long long)last,
                 (unsigned long long)size);
    } else if (range == web::RangeResult::kUnsatisfiable) {
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        snprintf(extra, sizeof(extra), "bytes */%llu",
                 (unsigned long long)size);
        httpd_resp_set_hdr(req, "Content-Range", extra);
        return httpd_resp_send(req, NULL, 0);
    }
    std::string disposition;
    if (download) {
        const size_t slash = relative.rfind('/');
        std::string name =
            slash == std::string::npos ? relative : relative.substr(slash + 1);
        for (char &c : name) {
            if (c == '"' || static_cast<unsigned char>(c) >= 0x80) {
                c = '_';
            }
        }
        disposition =
            "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
    }
    char header[320];
    const int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: video/mp4\r\nAccept-Ranges: bytes\r\n"
        "Content-Length: %llu\r\nCache-Control: no-store\r\n%s%s\r\n",
        status, (unsigned long long)(last - first + 1), extra,
        disposition.c_str());
    if (header_length <= 0 || header_length >= (int)sizeof(header)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "header");
    }
    std::unique_ptr<uint8_t, decltype(&free)> buffer(
        static_cast<uint8_t *>(heap_caps_malloc(
            WEB_MP4_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        free);
    if (buffer == nullptr) {
        buffer.reset(static_cast<uint8_t *>(malloc(WEB_MP4_CHUNK)));
    }
    if (buffer == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no memory");
    }
    const int fd = httpd_req_to_sockfd(req);
    if (!send_all(fd, header, (size_t)header_length)) {
        return ESP_FAIL;
    }
    for (uint64_t position = first; position <= last;) {
        const size_t want =
            (size_t)(last - position + 1 < WEB_MP4_CHUNK ? last - position + 1
                                                         : WEB_MP4_CHUNK);
        // A camera session needs the card; the length was announced, so the
        // connection ends.
        if (session_active()) {
            ESP_LOGW(TAG, "Stopped sending %s: a camera session started",
                     relative.c_str());
            return ESP_FAIL;
        }
        const int64_t got = file->read(position, buffer.get(), want);
        if (got <= 0 ||
            !send_all(fd, (const char *)buffer.get(), (size_t)got)) {
            return ESP_FAIL;
        }
        position += (uint64_t)got;
    }
    return ESP_OK;
}

// The name of the MP4 that the listing offers for the video file `name`
// (<base>.h264), or "" if it is no such file or an MP4 of that name exists.
std::string offered_mp4(const std::string &directory, const std::string &name)
{
    if (name.size() <= 5 ||
        strcasecmp(name.c_str() + name.size() - 5, ".h264") != 0) {
        return "";
    }
    const std::string mp4_name = name.substr(0, name.size() - 5) + ".mp4";
    struct stat info = {};
    if (stat((directory + "/" + mp4_name).c_str(), &info) == 0) {
        return "";
    }
    return mp4_name;
}
#endif

esp_err_t handle_file(httpd_req_t *req)
{
    std::string uri = req->uri + strlen("/files");
    const size_t query = uri.find('?');
    if (query != std::string::npos) {
        uri.resize(query);
    }
    std::string decoded;
    std::string relative;
    if (!web::url_decode(uri, decoded) ||
        !web::sanitize_path(decoded, relative) || relative.empty()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || S_ISDIR(info.st_mode)) {
#if CONFIG_CAMERA_VIDEO_MP4_ON_REQUEST
        std::string video;
        std::string audio;
        if (!S_ISDIR(info.st_mode) &&
            web::mp4_sources(relative, video, audio)) {
            return serve_mp4(req, relative,
                             query_long(req, "download", 0) != 0);
        }
#endif
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
    }
    FILE *file = fopen(path.c_str(), "rb");
    if (file == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
    }
    const size_t slash = relative.rfind('/');
    const std::string name =
        slash == std::string::npos ? relative : relative.substr(slash + 1);
    httpd_resp_set_type(req, web::content_type(name));
    std::string disposition;
    if (query_long(req, "download", 0) != 0) {
        std::string safe_name = name;
        for (char &c : safe_name) {
            if (c == '"' || static_cast<unsigned char>(c) >= 0x80) {
                c = '_';
            }
        }
        disposition = "attachment; filename=\"" + safe_name + "\"";
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    }
    const esp_err_t error = send_file_bytes(req, file, -1);
    fclose(file);
    return error;
}

esp_err_t handle_delete(httpd_req_t *req)
{
    std::string relative;
    if (!query_path(req, "path", relative) || relative.empty()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string path = absolute(relative);
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
    }
    if (S_ISDIR(info.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "directories cannot be deleted");
    }
    // Removing a file that is open for writing corrupts the FAT volume: the
    // log being written and unfinished downloads or catalog updates.
    char log_path[64];
    sd_log_current_path(log_path, sizeof(log_path));
    const bool in_use = path == log_path || web::is_temporary_file(relative);
    if (in_use) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "the file is being written");
    }
    if (remove(path.c_str()) != 0) {
        ESP_LOGW(TAG, "Could not delete %s: errno %d", path.c_str(), errno);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "delete failed");
    }
    ESP_LOGI(TAG, "Deleted %s", path.c_str());
    // The free space changed.
    s_sd_info_us = 0;
    return send_json(req, "{\"ok\":true}");
}

esp_err_t handle_log(httpd_req_t *req)
{
    char path[64];
    sd_log_current_path(path, sizeof(path));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    FILE *file = path[0] != '\0' ? fopen(path, "rb") : NULL;
    if (file == NULL) {
        httpd_resp_set_hdr(req, "X-Log-Size", "0");
        return httpd_resp_send(req, "", 0);
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    long offset = query_long(req, "offset", -1);
    if (offset < 0 || offset > size) {
        // First request or a new file: start with the end of the log.
        offset = size > WEB_LOG_MAX_READ ? size - WEB_LOG_MAX_READ : 0;
    }
    const long length =
        size - offset > WEB_LOG_MAX_READ ? WEB_LOG_MAX_READ : size - offset;
    char header[24];
    snprintf(header, sizeof(header), "%ld", offset + length);
    httpd_resp_set_hdr(req, "X-Log-Size", header);
    const char *relative = path + strlen(SD_CARD_MOUNT_POINT) + 1;
    httpd_resp_set_hdr(req, "X-Log-Path", relative);
    fseek(file, offset, SEEK_SET);
    const esp_err_t error = send_file_bytes(req, file, length);
    fclose(file);
    return error;
}

esp_err_t start_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.max_open_sockets = 5;
    config.stack_size = 6144;
    config.max_uri_handlers = 14;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "HTTP server");

    static const httpd_uri_t kHandlers[] = {
        {"/", HTTP_GET, handle_asset, const_cast<Asset *>(&kIndexHtml)},
        // The page itself, for the address of a directory or file on the card.
        {"/browse", HTTP_GET, handle_asset, const_cast<Asset *>(&kIndexHtml)},
        {"/browse/*", HTTP_GET, handle_asset, const_cast<Asset *>(&kIndexHtml)},
        {"/style.css", HTTP_GET, handle_asset, const_cast<Asset *>(&kStyleCss)},
        {"/app.js", HTTP_GET, handle_asset, const_cast<Asset *>(&kAppJs)},
        {"/api/status", HTTP_GET, handle_status, NULL},
        {"/api/search", HTTP_POST, handle_search, NULL},
        {"/api/stop", HTTP_POST, handle_stop, NULL},
        {"/api/time", HTTP_POST, handle_time, NULL},
        {"/api/files", HTTP_GET, handle_files, NULL},
        {"/api/log", HTTP_GET, handle_log, NULL},
        {"/files/*", HTTP_GET, handle_file, NULL},
        {"/api/delete", HTTP_POST, handle_delete, NULL},
    };
    for (const httpd_uri_t &handler : kHandlers) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handler), TAG,
                            "URI handler %s", handler.uri);
    }
    return ESP_OK;
}

}  // namespace

esp_err_t web_ui_start(const web_ui_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(start_network(), TAG, "network");
    return start_server();
}

#if CONFIG_WEB_UI_OVER_ETHERNET
// Called with s_lock held; there is no access point to open.
static void reopen_access_point()
{
}

// The Ethernet interface is not affected by the BLE connection setup.
void web_ui_suspend_access_point(bool suspend)
{
    (void)suspend;
}
#else
// Opens the suspended access point if it is wanted and the memory suffices.
// Called with s_lock held.
static void reopen_access_point()
{
    if (!s_ap_suspended || !s_ap_reopen_wanted) {
        return;
    }
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (block < WEB_AP_REOPEN_MIN_BLOCK) {
        ESP_LOGW(TAG,
                 "Access point stays closed: largest free internal block %u "
                 "B; trying again at the next status change",
                 (unsigned)block);
        return;
    }
    esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error == ESP_OK) {
        // The channel of the last camera, as at boot.
        s_ap_config.ap.channel = load_channel();
        error = esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open the access point: %s",
                 esp_err_to_name(error));
        return;
    }
    s_ap_suspended = false;
    ESP_LOGI(TAG, "Access point open again");
}

void web_ui_suspend_access_point(bool suspend)
{
    if (s_lock == NULL || !s_ap_started) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ap_reopen_wanted = !suspend;
    if (suspend && !s_ap_suspended) {
        const esp_err_t error = esp_wifi_set_mode(WIFI_MODE_STA);
        if (error == ESP_OK) {
            s_ap_suspended = true;
            ESP_LOGI(TAG, "Access point closed for the BLE connection setup");
        } else {
            ESP_LOGW(TAG, "Could not close the access point: %s",
                     esp_err_to_name(error));
        }
    } else if (!suspend) {
        reopen_access_point();
    }
    xSemaphoreGive(s_lock);
}
#endif

const char *web_ui_ssid(void)
{
    return s_ssid;
}

const char *web_ui_password(void)
{
    return s_password;
}

const char *web_ui_url(void)
{
    return s_url;
}

void web_ui_set_status(const char *phase, const char *ssid,
                       uint32_t retry_delay_ms)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The result stays visible after the session until the next search.
    if (strcmp(phase, "searching") == 0 && strcmp(s_phase, "searching") != 0) {
        s_media_done = 0;
        s_media_stored = 0;
        s_media_total = 0;
    }
    snprintf(s_phase, sizeof(s_phase), "%s", phase);
    if (strcmp(phase, "finished") == 0) {
        s_cameras_us = 0;  // the sync time on the card has just changed
    }
    reopen_access_point();
    snprintf(s_camera_ssid, sizeof(s_camera_ssid), "%s",
             ssid != NULL ? ssid : "");
    s_retry_deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(retry_delay_ms) * 1000;
    xSemaphoreGive(s_lock);
}

void web_ui_set_media_progress(uint32_t done, uint32_t stored, uint32_t total)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_media_done = done;
    s_media_stored = stored;
    s_media_total = total;
    xSemaphoreGive(s_lock);
}
