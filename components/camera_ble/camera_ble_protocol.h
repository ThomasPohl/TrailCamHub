#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_ble.h"

#define CAMERA_BLE_MAX_COMMAND_LEN 20

bool camera_ble_protocol_init(camera_ble_credentials_cb_t on_credentials);
uint8_t *camera_ble_protocol_command(size_t *length);
void camera_ble_protocol_reset_response(void);
void camera_ble_protocol_process_notification(const uint8_t *data,
                                              size_t data_len);
