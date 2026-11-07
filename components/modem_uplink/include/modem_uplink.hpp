#pragma once

// The backend uplink over the cellular modem: the A7670E of the Waveshare
// ESP32-S3-A7670E-4G, on the S3's native USB port (USB host, PPP). It is
// brought up for one sync and down again afterwards (docs/modem.md):
//
//   connect():    power on (GPIO, if wired), USB host and modem driver, radio
//                 on, SIM PIN, network registration, PPP data call; the PPP
//                 interface becomes the default route.
//   disconnect(): end the data call, radio off (flight mode), drivers
//                 removed, power off (GPIO).
//
// Both run in the component's own task and report through the callbacks,
// so the caller (the session controller) never blocks. A disconnect()
// cancels a connect() that is still running.

#include <string>

#include "esp_err.h"

namespace modem_uplink
{

struct Settings {
    std::string apn;      // empty: the network's default
    std::string sim_pin;  // empty: none
};

struct Callbacks {
    void (*connected)();
    void (*failed)(const char *reason);
};

// True if this build drives a modem (CONFIG_MODEM_UPLINK).
bool available();

// Creates the task. Call once, after the network stack and the default
// event loop are up.
esp_err_t init(const Callbacks &callbacks);

// Starts bringing the uplink up; the result comes through the callbacks.
esp_err_t connect(const Settings &settings);

// Takes the uplink down (also after a failed or cancelled connect()).
void disconnect();

// True from connect() until the matching disconnect() is done.
bool active();

}  // namespace modem_uplink
