#pragma once

#include <stdbool.h>

#include "esp_gap_ble_api.h"

#define CAMERA_BLE_TARGET_NAME "TC100_ED3B"
#define CAMERA_BLE_SERVICE_UUID 0x8801

bool camera_ble_is_connectable_advertisement(esp_ble_evt_type_t event_type);
bool camera_ble_is_candidate_advertisement(esp_ble_gap_cb_param_t *param);
