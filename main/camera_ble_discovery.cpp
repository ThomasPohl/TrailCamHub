/*
 * This module identifies BLE advertisements that may belong to the target
 * camera by checking connectability, advertised names, and service UUIDs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_gap_ble_api.h"

#include "camera_ble_discovery.h"

static const char *const s_camera_name_allowlist[] = {
    CAMERA_BLE_TARGET_NAME,
};
static const uint16_t s_camera_service_uuids[] = {
    CAMERA_BLE_SERVICE_UUID,
};

static bool name_matches_camera_allowlist(const uint8_t *name, size_t name_len)
{
    if (name == NULL || name_len == 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(s_camera_name_allowlist) /
                               sizeof(s_camera_name_allowlist[0]);
         i++) {
        const char *allowed_name = s_camera_name_allowlist[i];
        size_t allowed_name_len = strlen(allowed_name);

        if (name_len == allowed_name_len &&
            memcmp(name, allowed_name, name_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool advertisement_contains_uuid16(const uint8_t *adv_data,
                                          size_t adv_len, uint16_t target_uuid)
{
    for (size_t offset = 0; offset < adv_len;) {
        uint8_t field_len = adv_data[offset];
        if (field_len == 0) {
            break;
        }
        if (offset + 1 + field_len > adv_len) {
            break;
        }

        uint8_t ad_type = adv_data[offset + 1];
        const uint8_t *value = adv_data + offset + 2;
        size_t value_len = field_len - 1;

        if (ad_type == ESP_BLE_AD_TYPE_16SRV_CMPL ||
            ad_type == ESP_BLE_AD_TYPE_16SRV_PART) {
            for (size_t i = 0; i + 1 < value_len; i += 2) {
                uint16_t uuid = value[i] | ((uint16_t)value[i + 1] << 8);
                if (uuid == target_uuid) {
                    return true;
                }
            }
        }
        offset += field_len + 1;
    }
    return false;
}

bool camera_ble_is_connectable_advertisement(esp_ble_evt_type_t event_type)
{
    return event_type == ESP_BLE_EVT_CONN_ADV ||
           event_type == ESP_BLE_EVT_CONN_DIR_ADV;
}

bool camera_ble_is_candidate_advertisement(esp_ble_gap_cb_param_t *param)
{
    uint8_t name_len = 0;
    const uint8_t *name = esp_ble_resolve_adv_data(
        param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);

    if (name == NULL) {
        name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                        ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name != NULL && name_matches_camera_allowlist(name, name_len)) {
        return true;
    }

    size_t adv_len = param->scan_rst.adv_data_len;
    for (size_t i = 0;
         i < sizeof(s_camera_service_uuids) / sizeof(s_camera_service_uuids[0]);
         i++) {
        if (advertisement_contains_uuid16(param->scan_rst.ble_adv, adv_len,
                                          s_camera_service_uuids[i])) {
            return true;
        }
    }

    return false;
}
