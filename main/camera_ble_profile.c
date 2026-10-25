#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_gattc_api.h"
#include "esp_log.h"

#include "camera_ble_discovery.h"
#include "camera_ble_profile.h"

#define WIFI_CONTROL_CHAR_UUID 0x0002
#define WIFI_RESPONSE_CHAR_UUID 0x0003
#define MAX_DISCOVERED_SERVICES 32
#define MAX_NOTIFICATION_CHARACTERISTICS 16

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    esp_bt_uuid_t uuid;
} discovered_service_t;

static const char *TAG = "trailcamhub_ble";
static discovered_service_t s_discovered_services[MAX_DISCOVERED_SERVICES];
static size_t s_discovered_service_count;
static bool s_discovered_service_overflow;
static uint16_t s_service_start_handle;
static uint16_t s_service_end_handle;
static uint16_t s_notification_handles[MAX_NOTIFICATION_CHARACTERISTICS];
static uint16_t s_notification_configs[MAX_NOTIFICATION_CHARACTERISTICS];
static size_t s_notification_count;
static uint16_t s_wifi_response_handle;

static void log_gatt_uuid(const char *kind, const esp_bt_uuid_t *uuid)
{
    if (uuid->len == ESP_UUID_LEN_16) {
        ESP_LOGI(TAG, "Discovered GATT %s UUID=0x%04x", kind,
                 uuid->uuid.uuid16);
    } else if (uuid->len == ESP_UUID_LEN_32) {
        ESP_LOGI(TAG, "Discovered GATT %s UUID=0x%08lx", kind,
                 (unsigned long)uuid->uuid.uuid32);
    } else if (uuid->len == ESP_UUID_LEN_128) {
        ESP_LOGI(TAG, "Discovered GATT %s with a 128-bit UUID", kind);
        char formatted[3 * ESP_UUID_LEN_128 + 1];
        size_t formatted_len = 0;
        for (size_t i = 0; i < ESP_UUID_LEN_128; i++) {
            int written = snprintf(formatted + formatted_len,
                                   sizeof(formatted) - formatted_len,
                                   i == 0 ? "%02X" : " %02X",
                                   uuid->uuid.uuid128[i]);
            if (written < 0 ||
                (size_t)written >= sizeof(formatted) - formatted_len) {
                ESP_LOGE(TAG, "Could not format GATT UUID");
                return;
            }
            formatted_len += (size_t)written;
        }
        ESP_LOGI(TAG, "GATT UUID bytes (16 bytes): %s", formatted);
    } else {
        ESP_LOGW(TAG, "Discovered GATT %s with unsupported UUID length %u",
                 kind, (unsigned)uuid->len);
    }
}

static bool is_camera_service(const esp_bt_uuid_t *uuid)
{
    return uuid->len == ESP_UUID_LEN_16 &&
           uuid->uuid.uuid16 == CAMERA_BLE_SERVICE_UUID;
}

static void queue_characteristic_updates(
    const esp_gattc_char_elem_t *characteristic)
{
    uint16_t config = 0;

    if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0) {
        config |= 0x0001;
    }
    if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0) {
        config |= 0x0002;
    }
    if (config == 0) {
        return;
    }
    if (s_notification_count >= MAX_NOTIFICATION_CHARACTERISTICS) {
        ESP_LOGW(TAG, "Too many update characteristics; skipping handle %u",
                 characteristic->char_handle);
        return;
    }

    s_notification_handles[s_notification_count] = characteristic->char_handle;
    s_notification_configs[s_notification_count] = config;
    s_notification_count++;
}

void camera_ble_profile_reset(void)
{
    s_discovered_service_count = 0;
    s_discovered_service_overflow = false;
    s_service_start_handle = 0;
    s_service_end_handle = 0;
    s_notification_count = 0;
    s_wifi_response_handle = 0;
}

void camera_ble_profile_add_service(const esp_bt_uuid_t *uuid,
                                    uint16_t start_handle,
                                    uint16_t end_handle)
{
    log_gatt_uuid("service", uuid);

    bool service_already_recorded = false;
    for (size_t i = 0; i < s_discovered_service_count; i++) {
        if (s_discovered_services[i].start_handle == start_handle &&
            s_discovered_services[i].end_handle == end_handle) {
            service_already_recorded = true;
            break;
        }
    }

    if (!service_already_recorded) {
        if (s_discovered_service_count < MAX_DISCOVERED_SERVICES) {
            discovered_service_t *service =
                &s_discovered_services[s_discovered_service_count++];
            service->start_handle = start_handle;
            service->end_handle = end_handle;
            service->uuid = *uuid;
        } else {
            s_discovered_service_overflow = true;
            ESP_LOGW(TAG, "More than %u GATT services found; "
                     "remaining services cannot be inspected",
                     MAX_DISCOVERED_SERVICES);
        }
    }

    if (is_camera_service(uuid)) {
        s_service_start_handle = start_handle;
        s_service_end_handle = end_handle;
        ESP_LOGI(TAG, "Found camera service 0x%04x (handles %u-%u)",
                 CAMERA_BLE_SERVICE_UUID, s_service_start_handle,
                 s_service_end_handle);
    }
}

bool camera_ble_profile_discover_characteristics(esp_gatt_if_t gatt_if,
                                                 uint16_t conn_id)
{
    bool found_camera_service = false;
    bool has_wifi_control = false;
    bool has_wifi_response = false;

    if (s_discovered_service_overflow) {
        ESP_LOGE(TAG, "Too many GATT services to inspect safely");
        return false;
    }

    for (size_t service_index = 0;
         service_index < s_discovered_service_count; service_index++) {
        const discovered_service_t *service =
            &s_discovered_services[service_index];
        bool camera_service = is_camera_service(&service->uuid);
        esp_gattc_char_elem_t *characteristics;
        uint16_t count = 0;
        esp_err_t status = esp_ble_gattc_get_attr_count(
            gatt_if, conn_id, ESP_GATT_DB_CHARACTERISTIC,
            service->start_handle, service->end_handle,
            ESP_GATT_INVALID_HANDLE, &count);

        if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG,
                     "Could not count characteristics for GATT service "
                     "handles %u-%u: %d",
                     service->start_handle, service->end_handle, status);
            if (camera_service) {
                return false;
            }
            continue;
        }
        if (count == 0) {
            ESP_LOGI(TAG, "GATT service handles %u-%u has no characteristics",
                     service->start_handle, service->end_handle);
            if (camera_service) {
                found_camera_service = true;
            }
            continue;
        }

        characteristics = calloc(count, sizeof(*characteristics));
        if (characteristics == NULL) {
            ESP_LOGE(TAG, "Could not allocate memory for %u characteristics",
                     count);
            if (camera_service) {
                return false;
            }
            continue;
        }

        status = esp_ble_gattc_get_all_char(
            gatt_if, conn_id, service->start_handle, service->end_handle,
            characteristics, &count, 0);
        if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG,
                     "Could not read characteristics for GATT service "
                     "handles %u-%u: %d",
                     service->start_handle, service->end_handle, status);
            free(characteristics);
            if (camera_service) {
                return false;
            }
            continue;
        }

        ESP_LOGI(TAG, "GATT service handles %u-%u has %u characteristic(s)",
                 service->start_handle, service->end_handle, count);
        if (camera_service) {
            found_camera_service = true;
            s_service_start_handle = service->start_handle;
            s_service_end_handle = service->end_handle;
        }

        for (uint16_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "Characteristic handle=%u properties=0x%02x",
                     characteristics[i].char_handle,
                     characteristics[i].properties);
            log_gatt_uuid("characteristic", &characteristics[i].uuid);

            if (!camera_service ||
                characteristics[i].uuid.len != ESP_UUID_LEN_16) {
                continue;
            }
            if (characteristics[i].uuid.uuid.uuid16 ==
                    WIFI_CONTROL_CHAR_UUID &&
                (characteristics[i].properties &
                 (ESP_GATT_CHAR_PROP_BIT_WRITE |
                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) != 0) {
                has_wifi_control = true;
            } else if (characteristics[i].uuid.uuid.uuid16 ==
                           WIFI_RESPONSE_CHAR_UUID &&
                       (characteristics[i].properties &
                        (ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                         ESP_GATT_CHAR_PROP_BIT_INDICATE)) != 0) {
                has_wifi_response = true;
                s_wifi_response_handle = characteristics[i].char_handle;
                queue_characteristic_updates(&characteristics[i]);
            }
        }
        free(characteristics);
    }

    if (!found_camera_service || !has_wifi_control || !has_wifi_response) {
        ESP_LOGI(TAG,
                 "BLE device is not compatible: service 0x%04x has no "
                 "writable 0x%04x and notify/indicate 0x%04x pair",
                 CAMERA_BLE_SERVICE_UUID, WIFI_CONTROL_CHAR_UUID,
                 WIFI_RESPONSE_CHAR_UUID);
        s_notification_count = 0;
        s_wifi_response_handle = 0;
        return false;
    }

    ESP_LOGI(TAG, "BLE device matches the expected camera GATT profile");
    return true;
}

uint16_t camera_ble_profile_service_start_handle(void)
{
    return s_service_start_handle;
}

uint16_t camera_ble_profile_service_end_handle(void)
{
    return s_service_end_handle;
}

uint16_t camera_ble_profile_wifi_response_handle(void)
{
    return s_wifi_response_handle;
}

size_t camera_ble_profile_notification_count(void)
{
    return s_notification_count;
}

bool camera_ble_profile_notification(size_t index, uint16_t *handle,
                                     uint16_t *config)
{
    if (index >= s_notification_count) {
        return false;
    }
    *handle = s_notification_handles[index];
    *config = s_notification_configs[index];
    return true;
}

uint16_t camera_ble_profile_notification_config(uint16_t handle)
{
    for (size_t i = 0; i < s_notification_count; i++) {
        if (s_notification_handles[i] == handle) {
            return s_notification_configs[i];
        }
    }
    return 0;
}
