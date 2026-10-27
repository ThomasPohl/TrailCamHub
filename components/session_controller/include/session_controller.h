#pragma once

#include "esp_err.h"

/*
 * The session controller runs the camera workflow in its own task: activate
 * the camera over BLE, join its Wi-Fi, run the camera session, and retry
 * after failures with an increasing delay.
 */

/* Creates the controller's queues and timer and initializes camera Wi-Fi. */
esp_err_t session_controller_init(void);

/*
 * Starts camera BLE and the controller tasks. The Bluetooth controller and
 * Bluedroid must be enabled before.
 */
esp_err_t session_controller_start(void);
