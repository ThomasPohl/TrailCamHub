/*
 * This module keeps the ESP-Hosted firmware of the ESP32-C6 co-processor in
 * step with the host. The build embeds a C6 image made from the same
 * esp_hosted release as the host driver; at boot the image is flashed to the
 * C6 over the ESP-Hosted link when the C6 reports a different version.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "coprocessor_update.h"

extern const uint8_t c6_image_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t c6_image_end[] asm("_binary_network_adapter_bin_end");

#define OTA_CHUNK_SIZE 1400
#define NVS_NAMESPACE "cp_update"
#define NVS_KEY_VERSION "version"
#define NVS_KEY_ATTEMPTS "attempts"

static const char *TAG = "trailcamhub_cp";

// The OTA write API takes a mutable buffer; the image itself is in flash.
static uint8_t s_chunk[OTA_CHUNK_SIZE];

// Checks that the embedded data is an ESP32-C6 application image and copies
// the version from its application descriptor.
static esp_err_t read_image_version(char *version, size_t version_size)
{
    size_t image_size = c6_image_end - c6_image_start;
    size_t desc_offset =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (image_size < desc_offset + sizeof(esp_app_desc_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_image_header_t header;
    memcpy(&header, c6_image_start, sizeof(header));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC ||
        header.chip_id != ESP_CHIP_ID_ESP32C6) {
        return ESP_ERR_INVALID_VERSION;
    }
    esp_app_desc_t desc;
    memcpy(&desc, c6_image_start + desc_offset, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return ESP_ERR_INVALID_VERSION;
    }
    snprintf(version, version_size, "%s", desc.version);
    return ESP_OK;
}

// Returns how often the given image version was already sent to the C6.
static uint8_t read_attempts(nvs_handle_t nvs, const char *version)
{
    char stored[sizeof(esp_app_desc_t::version)] = {};
    size_t stored_size = sizeof(stored);
    uint8_t attempts = 0;
    if (nvs_get_str(nvs, NVS_KEY_VERSION, stored, &stored_size) != ESP_OK ||
        strcmp(stored, version) != 0 ||
        nvs_get_u8(nvs, NVS_KEY_ATTEMPTS, &attempts) != ESP_OK) {
        return 0;
    }
    return attempts;
}

static void write_attempts(nvs_handle_t nvs, const char *version,
                           uint8_t attempts)
{
    esp_err_t error = nvs_set_str(nvs, NVS_KEY_VERSION, version);
    if (error == ESP_OK) {
        error = nvs_set_u8(nvs, NVS_KEY_ATTEMPTS, attempts);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the update attempt: %s",
                 esp_err_to_name(error));
    }
}

static esp_err_t send_image(void)
{
    size_t image_size = c6_image_end - c6_image_start;
    esp_err_t error = esp_hosted_slave_ota_begin();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C6 rejected the update: %s", esp_err_to_name(error));
        return error;
    }

    size_t next_report = image_size / 4;
    for (size_t offset = 0; offset < image_size; offset += OTA_CHUNK_SIZE) {
        size_t length = image_size - offset;
        if (length > OTA_CHUNK_SIZE) {
            length = OTA_CHUNK_SIZE;
        }
        memcpy(s_chunk, c6_image_start + offset, length);
        error = esp_hosted_slave_ota_write(s_chunk, length);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Update failed at byte %u of %u: %s",
                     (unsigned)offset, (unsigned)image_size,
                     esp_err_to_name(error));
            esp_hosted_slave_ota_end();
            return error;
        }
        if (offset + length >= next_report && offset + length < image_size) {
            ESP_LOGI(TAG, "Sent %u of %u bytes", (unsigned)(offset + length),
                     (unsigned)image_size);
            next_report += image_size / 4;
        }
    }

    error = esp_hosted_slave_ota_end();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C6 did not accept the new image: %s",
                 esp_err_to_name(error));
    }
    return error;
}

esp_err_t coprocessor_update_run(void)
{
    char image_version[sizeof(esp_app_desc_t::version)];
    esp_err_t error = read_image_version(image_version, sizeof(image_version));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Embedded C6 firmware image is invalid: %s",
                 esp_err_to_name(error));
        return error;
    }

    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGE(TAG, "No ESP-Hosted link to the C6");
        return ESP_ERR_INVALID_STATE;
    }

    // Old C6 firmware may not answer the version request; it is updated.
    esp_hosted_coprocessor_fwver_t running = {};
    bool version_known =
        esp_hosted_get_coprocessor_fwversion(&running) == ESP_OK;
    char running_version[24] = "unknown";
    if (version_known) {
        snprintf(running_version, sizeof(running_version),
                 "%" PRIu32 ".%" PRIu32 ".%" PRIu32, running.major1,
                 running.minor1, running.patch1);
    }

    nvs_handle_t nvs;
    error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS: %s", esp_err_to_name(error));
        return error;
    }

    if (version_known && strcmp(running_version, image_version) == 0) {
        ESP_LOGI(TAG, "C6 runs ESP-Hosted %s, matching the host",
                 running_version);
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        return ESP_OK;
    }

    uint8_t attempts = read_attempts(nvs, image_version);
    if (attempts >= CONFIG_COPROCESSOR_UPDATE_MAX_ATTEMPTS) {
        ESP_LOGE(TAG,
                 "C6 still runs ESP-Hosted %s after %u attempts to install "
                 "%s; flash the C6 over UART",
                 running_version, attempts, image_version);
        nvs_close(nvs);
        return ESP_ERR_INVALID_STATE;
    }
    attempts++;
    write_attempts(nvs, image_version, attempts);
    nvs_close(nvs);

    ESP_LOGW(TAG,
             "Updating C6 from ESP-Hosted %s to %s (%u bytes, attempt %u of "
             "%d); do not power off",
             running_version, image_version,
             (unsigned)(c6_image_end - c6_image_start), attempts,
             CONFIG_COPROCESSOR_UPDATE_MAX_ATTEMPTS);
    error = send_image();
    if (error != ESP_OK) {
        return error;
    }

    // Firmware before ESP-Hosted 2.6 switches to the new image on its own.
    if (version_known &&
        (running.major1 > 2 || (running.major1 == 2 && running.minor1 > 5))) {
        error = esp_hosted_slave_ota_activate();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "C6 did not activate the new image: %s",
                     esp_err_to_name(error));
            return error;
        }
    }

    // The host driver resets the C6 at start-up, so it boots the new image.
    ESP_LOGW(TAG, "C6 firmware updated; restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
