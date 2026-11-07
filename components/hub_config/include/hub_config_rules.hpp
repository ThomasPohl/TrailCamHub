#pragma once

// Names, validation and display rules for the hub's stored settings (home
// Wi-Fi, cellular modem, backend access and the wake-up schedule). Pure logic,
// host-testable; storage in NVS lives in hub_config.cpp.

#include <cstddef>
#include <string>

namespace hub_config
{

enum class Key {
    wifi_ssid,
    wifi_password,
    api_url,
    device_key,
    // Camera read plan, e.g. "05:00-09:00=30,*=120" (see schedule_plan.hpp);
    // empty: the default of the build.
    camera_plan,
    // Backend check-in interval in minutes ("0": none on schedule); empty:
    // the default of the build.
    checkin_min,
    // How the hub reaches the backend: "wifi" or "modem"; empty: the home
    // Wi-Fi if one is set, otherwise the modem (see transport()).
    transport,
    // APN of the SIM card's provider for the modem; empty: the one the
    // network suggests.
    apn,
    // PIN of the SIM card (4-8 digits); empty: the card has no PIN.
    sim_pin,
};

constexpr Key kAllKeys[] = {
    Key::wifi_ssid,  Key::wifi_password, Key::api_url,
    Key::device_key, Key::camera_plan,   Key::checkin_min,
    Key::transport,  Key::apn,           Key::sim_pin};

struct Settings {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string api_url;
    std::string device_key;
    std::string camera_plan;
    std::string checkin_min;
    std::string transport;
    std::string apn;
    std::string sim_pin;
};

enum class Transport {
    wifi,   // the home Wi-Fi
    modem,  // the cellular modem (PPP)
};

// Name used on the serial console and as the NVS key (max. 15 characters).
const char *key_name(Key key);
bool key_from_name(const std::string &name, Key &out);
bool is_secret(Key key);

std::string &field(Settings &settings, Key key);
const std::string &field(const Settings &settings, Key key);

// Returns an empty string if `value` is acceptable for `key`, otherwise a
// short English reason.
std::string validate(Key key, const std::string &value);

// Value as shown on the console: secrets are never echoed, only whether they
// are set (and, for the device key, the camera id part before the dot).
std::string display_value(Key key, const std::string &value);

// How the hub reaches the backend. `modem_available`: the build drives a
// modem. "modem" without one, or "wifi", is the Wi-Fi; empty is the Wi-Fi if
// an SSID is set and the modem otherwise.
Transport transport(const Settings &settings, bool modem_available);
const char *transport_name(Transport transport);

// True if the hub has everything it needs to reach the backend: the backend
// settings and, over the Wi-Fi, the home Wi-Fi.
bool uplink_configured(const Settings &settings, bool modem_available = false);

// The backend URL without trailing slashes, ready for appending "/v1/...".
std::string api_base(const std::string &api_url);

}  // namespace hub_config
