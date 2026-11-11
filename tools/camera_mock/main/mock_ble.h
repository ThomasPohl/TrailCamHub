#pragma once

// BLE peripheral of the camera mock (NimBLE): advertises like the camera and
// exposes service 0x8801 with the write characteristic 0x0002 and the notify
// characteristic 0x0003. In C because NimBLE's GATT tables are built from C
// compound literals.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called in the NimBLE host task for every write to 0x0002; must not block.
typedef void (*mock_ble_write_cb_t)(uint16_t conn_handle, const uint8_t *data,
                                    size_t size);

// Starts NimBLE and advertises `adv` (raw advertising data, at most 31
// bytes) as connectable.
esp_err_t mock_ble_start(const uint8_t *adv, size_t adv_size,
                         const char *device_name, mock_ble_write_cb_t on_write);

// Sends one notification on 0x0003.
esp_err_t mock_ble_notify(uint16_t conn_handle, const uint8_t *data,
                          size_t size);

// True while a client is connected; `subscribed` if it enabled notifications.
bool mock_ble_connected(bool *subscribed);

#ifdef __cplusplus
}
#endif
