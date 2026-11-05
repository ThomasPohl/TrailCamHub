#pragma once

// Persistent hub settings (home Wi-Fi, backend URL, device key) in NVS.
// They are written over the serial console, typically from the web
// installer, and never leave the device except to the configured backend.

#include <string>

#include "esp_err.h"

#include "hub_config_rules.hpp"

namespace hub_config
{

// Reads all settings; missing ones are left empty.
esp_err_t load(Settings &out);

// Validates and stores one setting. An empty value erases it.
esp_err_t set(Key key, const std::string &value, std::string *reason);

// Erases all settings.
esp_err_t clear();

// True if this build drives a cellular modem (CONFIG_MODEM_UPLINK).
bool modem_available();

// transport() and uplink_configured() for this build.
Transport uplink_transport(const Settings &settings);
bool uplink_ready(const Settings &settings);

}  // namespace hub_config
