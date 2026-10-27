/*
 * This module identifies BLE advertisements that may belong to the target
 * camera by checking advertised names and service UUIDs.
 */

#include "camera_ble_discovery.h"

#include "ble_advertising.hpp"

static const char *const s_camera_name_allowlist[] = {
    CAMERA_BLE_TARGET_NAME,
};
static const uint16_t s_camera_service_uuids[] = {
    CAMERA_BLE_SERVICE_UUID,
};

bool camera_ble_is_candidate_advertisement(const uint8_t *data, size_t length)
{
    const uint8_t *name = NULL;
    size_t name_len = 0;
    if (camera::ble::find_device_name(data, length, &name, &name_len) &&
        camera::ble::name_in_allowlist(
            name, name_len, s_camera_name_allowlist,
            sizeof(s_camera_name_allowlist) /
                sizeof(s_camera_name_allowlist[0]))) {
        return true;
    }

    for (size_t i = 0;
         i < sizeof(s_camera_service_uuids) / sizeof(s_camera_service_uuids[0]);
         i++) {
        if (camera::ble::contains_uuid16(data, length,
                                         s_camera_service_uuids[i])) {
            return true;
        }
    }
    return false;
}
