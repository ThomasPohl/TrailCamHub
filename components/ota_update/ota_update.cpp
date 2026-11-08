/*
 * Firmware update over HTTPS into the other OTA slot (see ota_update.hpp).
 * The image is streamed: each block is hashed and written to flash as it
 * arrives, nothing is kept in RAM. What the hub knows about earlier checks
 * and failed installs is kept in NVS (namespace "ota").
 */

#include "ota_update.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <strings.h>
#include <time.h>
#include <vector>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "ota_policy.hpp"
#include "sd_log.h"
#include "time_sync.h"

namespace ota
{

namespace
{

const char *TAG = "trailcamhub_ota";
constexpr int kTimeoutMs = 30000;
constexpr int kMaxRedirects = 5;
constexpr std::size_t kBlock = 4096;
// A broken download is continued this often with a range request.
constexpr int kMaxResumes = 3;
constexpr int kMaxManifestBytes = 16 * 1024;
const char *kNamespace = "ota";
const char *kCheckedAtKey = "checked_at";
const char *kFailVersionKey = "fail_ver";
const char *kFailCountKey = "fail_n";
const char *kFailAtKey = "fail_at";

std::atomic<bool> s_restart_pending{false};
std::atomic<bool> s_force_check{false};
std::mutex s_text_lock;
std::string s_last_check_text = "no update check since the start";
// The image header, the first segment header and the app description: the
// version of the image is known before anything is written.
constexpr std::size_t kDescriptionEnd = sizeof(esp_image_header_t) +
                                        sizeof(esp_image_segment_header_t) +
                                        sizeof(esp_app_desc_t);

bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308;
}

// Opens the URL for reading and follows redirects (GitHub release assets
// redirect to their storage host). Returns the content length (-1 unknown).
esp_err_t open_following_redirects(esp_http_client_handle_t client,
                                   int64_t &length, int &status)
{
    for (int i = 0; i <= kMaxRedirects; ++i) {
        esp_err_t error = esp_http_client_open(client, 0);
        if (error != ESP_OK) {
            return error;
        }
        length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (!is_redirect(status)) {
            return ESP_OK;
        }
        esp_http_client_flush_response(client, nullptr);
        error = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_ERR_HTTP_MAX_REDIRECT;
}

esp_http_client_handle_t make_client(const char *url)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    // Signed redirect URLs of release assets are long.
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = false;
    return esp_http_client_init(&config);
}

struct ClientCleanup {
    esp_http_client_handle_t client;
    ~ClientCleanup()
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
};

// Opens `url` from byte `from` on (a range request when from > 0) and follows
// redirects. Expects 200 for the whole file and 206 for a range.
esp_err_t open_from(esp_http_client_handle_t client, const std::string &url,
                    std::int64_t from, std::int64_t &length, std::string &error)
{
    esp_http_client_close(client);
    esp_http_client_set_url(client, url.c_str());
    if (from > 0) {
        const std::string range =
            "bytes=" + std::to_string((long long)from) + "-";
        esp_http_client_set_header(client, "Range", range.c_str());
    } else {
        esp_http_client_delete_header(client, "Range");
    }
    int status = 0;
    const esp_err_t result = open_following_redirects(client, length, status);
    if (result != ESP_OK) {
        error = std::string("download failed: ") + esp_err_to_name(result);
        return result;
    }
    const int expected = from > 0 ? 206 : 200;
    if (status != expected) {
        error = "download failed: HTTP " + std::to_string(status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

std::int64_t now_unix()
{
    return time_sync_clock_valid() ? (std::int64_t)time(nullptr) : 0;
}

void set_last_check_text(const std::string &text)
{
    std::lock_guard<std::mutex> guard(s_text_lock);
    s_last_check_text = text;
}

// --- NVS ---

std::int64_t load_i64(nvs_handle_t handle, const char *key)
{
    int64_t value = 0;
    return nvs_get_i64(handle, key, &value) == ESP_OK ? value : 0;
}

std::int64_t load_checked_at()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    const std::int64_t value = load_i64(handle, kCheckedAtKey);
    nvs_close(handle);
    return value;
}

Attempts load_attempts()
{
    Attempts attempts;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return attempts;
    }
    char version[32] = {};
    size_t length = sizeof(version);
    std::uint32_t failures = 0;
    if (nvs_get_str(handle, kFailVersionKey, version, &length) == ESP_OK &&
        nvs_get_u32(handle, kFailCountKey, &failures) == ESP_OK) {
        attempts.version = version;
        attempts.failures = failures;
        attempts.last_failure = load_i64(handle, kFailAtKey);
    }
    nvs_close(handle);
    return attempts;
}

void store(void (*write)(nvs_handle_t, const void *), const void *data)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        write(handle, data);
        error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the update state: %s",
                 esp_err_to_name(error));
    }
}

void store_checked_at(std::int64_t when)
{
    store(
        [](nvs_handle_t handle, const void *data) {
            nvs_set_i64(handle, kCheckedAtKey,
                        *static_cast<const std::int64_t *>(data));
        },
        &when);
}

void store_attempts(const Attempts &attempts)
{
    store(
        [](nvs_handle_t handle, const void *data) {
            const Attempts &a = *static_cast<const Attempts *>(data);
            if (a.failures == 0) {
                nvs_erase_key(handle, kFailVersionKey);
                nvs_erase_key(handle, kFailCountKey);
                nvs_erase_key(handle, kFailAtKey);
                return;
            }
            nvs_set_str(handle, kFailVersionKey, a.version.c_str());
            nvs_set_u32(handle, kFailCountKey, a.failures);
            nvs_set_i64(handle, kFailAtKey, a.last_failure);
        },
        &attempts);
}

Policy policy()
{
    Policy policy;
#if CONFIG_OTA_AUTO_UPDATE
    policy.min_battery_percent = CONFIG_OTA_MIN_BATTERY_PERCENT;
    policy.min_signal_dbm = CONFIG_OTA_MODEM_MIN_SIGNAL_DBM;
    policy.max_attempts = CONFIG_OTA_MAX_ATTEMPTS;
    policy.retry_after_s = (std::int64_t)CONFIG_OTA_RETRY_AFTER_H * 3600;
    policy.check_interval_s = (std::int64_t)CONFIG_OTA_CHECK_INTERVAL_H * 3600;
#endif
    return policy;
}

// Reads the release manifest and the image for this board.
esp_err_t fetch_offer(Offer &offer, std::string &error)
{
#if CONFIG_OTA_AUTO_UPDATE
    const char *url = CONFIG_OTA_MANIFEST_URL;
#else
    const char *url = "";
#endif
    if (url[0] == '\0') {
        error = "no manifest URL";
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_handle_t client = make_client(url);
    if (client == nullptr) {
        error = "no memory";
        return ESP_ERR_NO_MEM;
    }
    ClientCleanup cleanup{client};
    std::int64_t length = -1;
    esp_err_t result = open_from(client, url, 0, length, error);
    if (result != ESP_OK) {
        error = "manifest: " + error;
        return result;
    }
    if (length > kMaxManifestBytes) {
        error = "manifest too large";
        return ESP_ERR_INVALID_SIZE;
    }
    std::string body;
    char buffer[512];
    while ((int)body.size() <= kMaxManifestBytes) {
        const int n = esp_http_client_read(client, buffer, sizeof(buffer));
        if (n < 0) {
            error = "manifest: download interrupted";
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        body.append(buffer, (std::size_t)n);
    }
    if ((int)body.size() > kMaxManifestBytes) {
        error = "manifest too large";
        return ESP_ERR_INVALID_SIZE;
    }
    const std::string key = std::string(board()) + "-" + variant();
    if (!parse_manifest(body, key, offer)) {
        error = "the manifest has no valid image " + key;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

struct Sha256 {
    mbedtls_sha256_context context;
    Sha256()
    {
        mbedtls_sha256_init(&context);
        mbedtls_sha256_starts(&context, 0);
    }
    ~Sha256()
    {
        mbedtls_sha256_free(&context);
    }
    void update(const std::uint8_t *data, std::size_t length)
    {
        mbedtls_sha256_update(&context, data, length);
    }
    std::string hex()
    {
        std::uint8_t digest[32];
        mbedtls_sha256_finish(&context, digest);
        return to_hex(digest, sizeof(digest));
    }
};

// Checks the description at the start of the image: the same project and
// the offered version.
bool check_description(const std::uint8_t *head, const Offer &offer,
                       std::string &error)
{
    esp_app_desc_t description;
    std::memcpy(&description,
                head + sizeof(esp_image_header_t) +
                    sizeof(esp_image_segment_header_t),
                sizeof(description));
    if (description.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        error = "not a firmware image";
        return false;
    }
    const esp_app_desc_t *running = esp_app_get_description();
    if (std::strncmp(description.project_name, running->project_name,
                     sizeof(description.project_name)) != 0) {
        error = "image of another project";
        return false;
    }
    if (std::strncmp(description.version, offer.version.c_str(),
                     sizeof(description.version)) != 0) {
        error = std::string("image has version ") +
                std::string(
                    description.version,
                    strnlen(description.version, sizeof(description.version)));
        return false;
    }
    return true;
}

}  // namespace

const char *board()
{
    return CONFIG_HUB_BOARD_ID;
}

const char *variant()
{
    return CONFIG_HUB_BUILD_VARIANT;
}

std::string running_version()
{
    return esp_app_get_description()->version;
}

std::string rolled_back_version()
{
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t description;
    if (invalid == nullptr ||
        esp_ota_get_partition_description(invalid, &description) != ESP_OK) {
        return "";
    }
    return std::string(
        description.version,
        strnlen(description.version, sizeof(description.version)));
}

bool automatic()
{
#if CONFIG_OTA_AUTO_UPDATE
    return true;
#else
    return false;
#endif
}

void log_boot_state()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr) {
        esp_ota_get_state_partition(running, &state);
    }
    ESP_LOGI(TAG, "Firmware %s (%s, %s) in slot %s", running_version().c_str(),
             board(), variant(), running != nullptr ? running->label : "?");
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "New firmware: it is kept after a successful check-in, "
                      "otherwise the next restart goes back to the old one");
    }
    const std::string rolled_back = rolled_back_version();
    if (!rolled_back.empty()) {
        ESP_LOGW(TAG, "Firmware %s was rolled back", rolled_back.c_str());
    }
}

void confirm_running()
{
    static bool confirmed = false;
    if (confirmed) {
        return;
    }
    confirmed = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Firmware %s confirmed after a successful check-in",
                     running_version().c_str());
        } else {
            ESP_LOGE(TAG, "Could not confirm the firmware: %s",
                     esp_err_to_name(error));
            confirmed = false;
        }
    }
}

esp_err_t install(const Offer &offer, std::string &error)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        error = "no OTA slot";
        return ESP_ERR_NOT_FOUND;
    }
    if (offer.size <= (std::int64_t)kDescriptionEnd ||
        (std::uint64_t)offer.size > target->size) {
        error = "image does not fit the OTA slot";
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = make_client(offer.url.c_str());
    if (client == nullptr) {
        error = "no memory";
        return ESP_ERR_NO_MEM;
    }
    ClientCleanup cleanup{client};

    const int64_t started_us = esp_timer_get_time();
    int64_t length = -1;
    esp_err_t result = open_from(client, offer.url, 0, length, error);
    if (result != ESP_OK) {
        return result;
    }
    if (length >= 0 && length != offer.size) {
        error = "download has " + std::to_string((long long)length) +
                " bytes, expected " + std::to_string((long long)offer.size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Installing firmware %s (%lld bytes) into slot %s",
             offer.version.c_str(), (long long)offer.size, target->label);
    esp_ota_handle_t handle = 0;
    result = esp_ota_begin(target, (size_t)offer.size, &handle);
    if (result != ESP_OK) {
        error = std::string("cannot erase the OTA slot: ") +
                esp_err_to_name(result);
        return result;
    }

    std::vector<std::uint8_t> block(kBlock);
    Sha256 sha;
    std::int64_t total = 0;
    std::int64_t logged = 0;
    bool checked = false;
    std::size_t pending = 0;  // bytes in `block` before the description check
    int resumes = 0;
    while (total < offer.size) {
        const int n = esp_http_client_read(
            client, reinterpret_cast<char *>(block.data()) + pending,
            (int)(block.size() - pending));
        if (n <= 0) {
            // Broken off: continue where the flash writes are (bytes not
            // written yet are fetched again).
            const char *what = n < 0 ? "interrupted" : "stalled";
            if (resumes >= kMaxResumes) {
                error = std::string("download ") + what + " at " +
                        std::to_string((long long)total) + " bytes";
                result = n < 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
                break;
            }
            resumes++;
            ESP_LOGW(TAG, "Download %s at %lld bytes; continuing (%d of %d)",
                     what, (long long)total, resumes, kMaxResumes);
            pending = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            std::int64_t rest = -1;
            result = open_from(client, offer.url, total, rest, error);
            if (result != ESP_OK) {
                break;
            }
            if (rest >= 0 && rest != offer.size - total) {
                error = "the continued download has " +
                        std::to_string((long long)rest) + " bytes, expected " +
                        std::to_string((long long)(offer.size - total));
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            continue;
        }
        std::size_t available = pending + (std::size_t)n;
        if (!checked) {
            if (available < kDescriptionEnd) {
                pending = available;
                continue;
            }
            if (!check_description(block.data(), offer, error)) {
                result = ESP_ERR_INVALID_VERSION;
                break;
            }
            checked = true;
        }
        if (total + (std::int64_t)available > offer.size) {
            error = "download is longer than announced";
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        sha.update(block.data(), available);
        result = esp_ota_write(handle, block.data(), available);
        if (result != ESP_OK) {
            error =
                std::string("flash write failed: ") + esp_err_to_name(result);
            break;
        }
        total += (std::int64_t)available;
        pending = 0;
        if (total - logged >= 256 * 1024) {
            logged = total;
            ESP_LOGI(TAG, "%lld of %lld bytes", (long long)total,
                     (long long)offer.size);
        }
    }
    if (result == ESP_OK && total != offer.size) {
        error = "download has " + std::to_string((long long)total) +
                " bytes, expected " + std::to_string((long long)offer.size);
        result = ESP_ERR_INVALID_SIZE;
    }
    if (result == ESP_OK) {
        const std::string digest = sha.hex();
        if (strcasecmp(digest.c_str(), offer.sha256.c_str()) != 0) {
            error = "SHA-256 does not match";
            result = ESP_ERR_INVALID_CRC;
        }
    }
    if (result != ESP_OK) {
        esp_ota_abort(handle);
        return result;
    }
    // Checks the image (segments, its own hash) before it can be booted.
    result = esp_ota_end(handle);
    if (result != ESP_OK) {
        error = std::string("image check failed: ") + esp_err_to_name(result);
        return result;
    }
    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        error = std::string("cannot select the new slot: ") +
                esp_err_to_name(result);
        return result;
    }
    ESP_LOGI(TAG,
             "Firmware %s installed in %lld ms (%d continued); it boots at "
             "the next restart",
             offer.version.c_str(),
             (long long)((esp_timer_get_time() - started_us) / 1000), resumes);
    return ESP_OK;
}

void check_for_update(const Conditions &conditions)
{
    const Policy rules = policy();
    const std::string running = running_version();
    const Decision first = ready(automatic(), running, conditions, rules);
    if (first == Decision::disabled || first == Decision::dev_build) {
        return;  // nothing to log at every sync
    }
    if (first != Decision::install) {
        ESP_LOGI(TAG, "No update check: %s", decision_text(first));
        set_last_check_text(std::string("update check skipped: ") +
                            decision_text(first));
        return;
    }
    const std::int64_t now = now_unix();
    const bool forced = s_force_check.exchange(false);
    if (!forced && !manifest_due(load_checked_at(), now, rules)) {
        return;
    }
    // Counted also when the read fails, so that an unreachable GitHub does
    // not cost data at every check-in.
    store_checked_at(now);

    Offer offer;
    std::string error;
    if (fetch_offer(offer, error) != ESP_OK) {
        ESP_LOGW(TAG, "Update check failed: %s", error.c_str());
        set_last_check_text("update check failed: " + error);
        return;
    }
    Attempts attempts = load_attempts();
    const Decision decision =
        decide(offer.version, running, rolled_back_version(), automatic(),
               conditions, rules, attempts, now);
    if (decision != Decision::install) {
        ESP_LOGI(TAG, "Release %s not installed: %s", offer.version.c_str(),
                 decision_text(decision));
        set_last_check_text("release " + offer.version +
                            " not installed: " + decision_text(decision));
        return;
    }
    if (install(offer, error) != ESP_OK) {
        record_failure(attempts, offer.version, now);
        store_attempts(attempts);
        ESP_LOGE(TAG, "Update to %s failed (%u in a row): %s",
                 offer.version.c_str(), (unsigned)attempts.failures,
                 error.c_str());
        set_last_check_text("update to " + offer.version + " failed: " + error);
        return;
    }
    store_attempts(Attempts{});
    set_last_check_text("release " + offer.version +
                        " installed; restarting after the cycle");
    s_restart_pending = true;
}

void force_check()
{
    s_force_check = true;
}

bool restart_pending()
{
    return s_restart_pending.load();
}

std::string last_check_text()
{
    std::lock_guard<std::mutex> guard(s_text_lock);
    return s_last_check_text;
}

void restart()
{
    ESP_LOGW(TAG, "Restarting into the new firmware");
#if CONFIG_SD_LOG_ENABLE
    sd_log_flush(3000);
#endif
    esp_restart();
}

}  // namespace ota
