/*
 * This module identifies BLE advertisements that may belong to the target
 * camera by checking connectability, advertised names, and service UUIDs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"

#include "ble_advertising.hpp"
#include "camera_ble_discovery.h"

static const char *const s_camera_name_allowlist[] = {
    CAMERA_BLE_TARGET_NAME,
};
static const uint16_t s_camera_service_uuids[] = {
    CAMERA_BLE_SERVICE_UUID,
};

bool camera_ble_is_connectable_advertisement(esp_ble_evt_type_t event_type)
{
    return event_type == ESP_BLE_EVT_CONN_ADV ||
           event_type == ESP_BLE_EVT_CONN_DIR_ADV;
}

bool camera_ble_is_candidate_advertisement(esp_ble_gap_cb_param_t *param)
{
    const uint8_t *data = param->scan_rst.ble_adv;
    size_t adv_len = param->scan_rst.adv_data_len;
    size_t total_len = adv_len + param->scan_rst.scan_rsp_len;
    if (total_len > sizeof(param->scan_rst.ble_adv)) {
        total_len = sizeof(param->scan_rst.ble_adv);
    }

    // The name may be in the advertisement or the appended scan response.
    const uint8_t *name = NULL;
    size_t name_len = 0;
    if (camera::ble::find_device_name(data, total_len, &name, &name_len) &&
        camera::ble::name_in_allowlist(
            name, name_len, s_camera_name_allowlist,
            sizeof(s_camera_name_allowlist) /
                sizeof(s_camera_name_allowlist[0]))) {
        return true;
    }

    for (size_t i = 0;
         i < sizeof(s_camera_service_uuids) / sizeof(s_camera_service_uuids[0]);
         i++) {
        if (camera::ble::contains_uuid16(data, adv_len,
                                         s_camera_service_uuids[i])) {
            return true;
        }
    }
    return false;
}
