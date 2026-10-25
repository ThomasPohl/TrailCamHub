#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gattc_api.h"

void camera_ble_profile_reset(void);
void camera_ble_profile_add_service(const esp_bt_uuid_t *uuid,
                                    uint16_t start_handle,
                                    uint16_t end_handle);
bool camera_ble_profile_discover_characteristics(esp_gatt_if_t gatt_if,
                                                 uint16_t conn_id);
uint16_t camera_ble_profile_service_start_handle(void);
uint16_t camera_ble_profile_service_end_handle(void);
uint16_t camera_ble_profile_wifi_response_handle(void);
size_t camera_ble_profile_notification_count(void);
bool camera_ble_profile_notification(size_t index, uint16_t *handle,
                                     uint16_t *config);
uint16_t camera_ble_profile_notification_config(uint16_t handle);
