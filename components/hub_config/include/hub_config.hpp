#pragma once

// Persistent hub settings (home Wi-Fi, backend URL, device key) in NVS.
// They are written over the serial console, typically from the web
// installer, and never leave the device except to the configured backend.

#include <cstdint>
#include <string>

#include "esp_err.h"

#include "hub_config_rules.hpp"

namespace hub_config
{

// Reads all settings; missing ones are left empty.
esp_err_t load(Settings &out);

// Validates and stores one setting. An empty value erases it.
esp_err_t set(Key key, const std::string &value, std::string *reason);

// Erases all settings (and a refused SIM PIN).
esp_err_t clear();

// The fingerprint of the SIM PIN the card refused last (see
// sim_pin_decision()); 0 if none. Storing a new sim_pin forgets it.
std::uint32_t refused_sim_pin();
esp_err_t set_refused_sim_pin(std::uint32_t fingerprint);

// True if this build drives a cellular modem (CONFIG_MODEM_UPLINK).
bool modem_available();

// transport() for this build.
Transport uplink_transport(const Settings &settings);
// uplink_configured() for this build, and the mode pushes (checks in).
bool uplink_ready(const Settings &settings);

// The capabilities and preset of this build (Kconfig), and the mode that
// applies with `settings`.
const Capabilities &capabilities();
const Preset &preset();
Mode mode(const Settings &settings);

}  // namespace hub_config
