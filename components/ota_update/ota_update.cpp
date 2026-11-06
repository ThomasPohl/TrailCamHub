/*
 * Firmware update over HTTPS into the other OTA slot (see ota_update.hpp).
 * The image is streamed: each block is hashed and written to flash as it
 * arrives, nothing is kept in RAM.
 */

#include "ota_update.hpp"

#include <cstring>
#include <memory>
#include <strings.h>
#include <vector>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "ota_policy.hpp"
#include "sd_log.h"

namespace ota
{

namespace
{

const char *TAG = "trailcamhub_ota";
constexpr int kTimeoutMs = 30000;
constexpr int kMaxRedirects = 5;
constexpr std::size_t kBlock = 4096;
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

    esp_http_client_config_t config = {};
    config.url = offer.url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    // Signed redirect URLs of release assets are long.
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        error = "no memory";
        return ESP_ERR_NO_MEM;
    }
    struct Cleanup {
        esp_http_client_handle_t client;
        ~Cleanup()
        {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
    } cleanup{client};

    const int64_t started_us = esp_timer_get_time();
    int64_t length = -1;
    int status = 0;
    esp_err_t result = open_following_redirects(client, length, status);
    if (result != ESP_OK) {
        error = std::string("download failed: ") + esp_err_to_name(result);
        return result;
    }
    if (status != 200) {
        error = "download failed: HTTP " + std::to_string(status);
        return ESP_FAIL;
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
    while (total < offer.size) {
        const int n = esp_http_client_read(
            client, reinterpret_cast<char *>(block.data()) + pending,
            (int)(block.size() - pending));
        if (n < 0) {
            error = "download interrupted";
            result = ESP_FAIL;
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            error = "download stalled";
            result = ESP_ERR_TIMEOUT;
            break;
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
             "Firmware %s installed in %lld ms; it boots at the next "
             "restart",
             offer.version.c_str(),
             (long long)((esp_timer_get_time() - started_us) / 1000));
    return ESP_OK;
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
