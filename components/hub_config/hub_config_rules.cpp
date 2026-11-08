#include "hub_config_rules.hpp"

#include "schedule_plan.hpp"

namespace hub_config
{

namespace
{

bool is_key_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool has_control_chars(const std::string &value)
{
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char *key_name(Key key)
{
    switch (key) {
    case Key::wifi_ssid:
        return "wifi_ssid";
    case Key::wifi_password:
        return "wifi_password";
    case Key::api_url:
        return "api_url";
    case Key::device_key:
        return "device_key";
    case Key::camera_plan:
        return "camera_plan";
    case Key::checkin_min:
        return "checkin_min";
    case Key::transport:
        return "transport";
    case Key::apn:
        return "apn";
    case Key::sim_pin:
        return "sim_pin";
    }
    return "unknown";
}

bool key_from_name(const std::string &name, Key &out)
{
    for (Key key : kAllKeys) {
        if (name == key_name(key)) {
            out = key;
            return true;
        }
    }
    return false;
}

bool is_secret(Key key)
{
    return key == Key::wifi_password || key == Key::device_key ||
           key == Key::sim_pin;
}

std::string &field(Settings &settings, Key key)
{
    switch (key) {
    case Key::wifi_ssid:
        return settings.wifi_ssid;
    case Key::wifi_password:
        return settings.wifi_password;
    case Key::api_url:
        return settings.api_url;
    case Key::camera_plan:
        return settings.camera_plan;
    case Key::checkin_min:
        return settings.checkin_min;
    case Key::transport:
        return settings.transport;
    case Key::apn:
        return settings.apn;
    case Key::sim_pin:
        return settings.sim_pin;
    case Key::device_key:
        break;
    }
    return settings.device_key;
}

const std::string &field(const Settings &settings, Key key)
{
    return field(const_cast<Settings &>(settings), key);
}

std::string validate(Key key, const std::string &value)
{
    switch (key) {
    case Key::wifi_ssid:
        if (value.empty() || value.size() > 32) {
            return "SSID must be 1-32 bytes";
        }
        return "";
    case Key::wifi_password:
        // Empty = open network; WPA passphrases are 8-63 characters, a raw
        // PSK is 64 hex digits.
        if (!value.empty() && (value.size() < 8 || value.size() > 64)) {
            return "password must be empty or 8-64 characters";
        }
        return "";
    case Key::api_url:
        if (value.rfind("https://", 0) != 0 || value.size() <= 8) {
            return "API URL must start with https://";
        }
        if (value.size() > 200 || has_control_chars(value) ||
            value.find(' ') != std::string::npos) {
            return "API URL is too long or contains invalid characters";
        }
        return "";
    case Key::device_key: {
        // <hubId>.<secret>, as printed by backend/scripts/register-hub.mjs
        const std::size_t dot = value.find('.');
        if (dot == std::string::npos || dot == 0 || dot > 64) {
            return "device key must look like <hubId>.<secret>";
        }
        const std::size_t secret_len = value.size() - dot - 1;
        if (secret_len < 16 || secret_len > 128) {
            return "device key secret must be 16-128 characters";
        }
        for (std::size_t i = 0; i < value.size(); i++) {
            if (i != dot && !is_key_char(value[i])) {
                return "device key contains invalid characters";
            }
        }
        return "";
    }
    case Key::camera_plan: {
        schedule::Plan plan;
        std::string reason;
        if (value.size() > 200 || has_control_chars(value)) {
            return "camera plan is too long or contains invalid characters";
        }
        if (!schedule::parse_plan(value, plan, &reason)) {
            return "camera plan: " + reason;
        }
        return "";
    }
    case Key::checkin_min: {
        if (value.empty() || value.size() > 4) {
            return "check-in interval must be 0-1440 minutes";
        }
        unsigned long minutes = 0;
        for (char c : value) {
            if (c < '0' || c > '9') {
                return "check-in interval must be 0-1440 minutes";
            }
            minutes = minutes * 10 + (unsigned long)(c - '0');
        }
        if (minutes > schedule::kMaxIntervalMin) {
            return "check-in interval must be 0-1440 minutes";
        }
        return "";
    }
    case Key::transport:
        if (value != "wifi" && value != "modem") {
            return "transport must be wifi or modem";
        }
        return "";
    case Key::apn:
        // Goes into AT+CGDCONT="..." as it is: only the characters of APN
        // names (labels of letters, digits and hyphens, joined by dots).
        if (value.empty() || value.size() > 63) {
            return "APN must be 1-63 characters";
        }
        for (char c : value) {
            if (!is_key_char(c) && c != '.') {
                return "APN may only contain letters, digits, '.', '-', '_'";
            }
        }
        return "";
    case Key::sim_pin:
        // The modem driver (at_cmd_set_pin) sends 4-digit PINs only.
        if (value.size() != 4) {
            return "SIM PIN must be 4 digits";
        }
        for (char c : value) {
            if (c < '0' || c > '9') {
                return "SIM PIN must be 4 digits";
            }
        }
        return "";
    }
    return "unknown setting";
}

std::string display_value(Key key, const std::string &value)
{
    if (value.empty()) {
        return key == Key::camera_plan || key == Key::checkin_min ||
                       key == Key::transport || key == Key::apn
                   ? "(default)"
                   : "(not set)";
    }
    if (key == Key::wifi_password || key == Key::sim_pin) {
        return "(set)";
    }
    if (key == Key::device_key) {
        const std::size_t dot = value.find('.');
        return value.substr(0, dot) + ".*** (set)";
    }
    return value;
}

Transport transport(const Settings &settings, bool modem_available)
{
    if (!modem_available || settings.transport == "wifi") {
        return Transport::wifi;
    }
    if (settings.transport == "modem") {
        return Transport::modem;
    }
    return settings.wifi_ssid.empty() ? Transport::modem : Transport::wifi;
}

const char *transport_name(Transport transport)
{
    return transport == Transport::modem ? "modem" : "wifi";
}

bool uplink_configured(const Settings &settings, bool modem_available)
{
    if (!validate(Key::api_url, settings.api_url).empty() ||
        !validate(Key::device_key, settings.device_key).empty()) {
        return false;
    }
    if (transport(settings, modem_available) == Transport::modem) {
        return true;
    }
    return validate(Key::wifi_ssid, settings.wifi_ssid).empty() &&
           validate(Key::wifi_password, settings.wifi_password).empty();
}

std::string api_base(const std::string &api_url)
{
    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

std::uint32_t sim_pin_fingerprint(const std::string &pin)
{
    // FNV-1a. Not a secret: the PIN itself is stored next to it.
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : std::string("sim_pin:") + pin) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash == 0 ? 1 : hash;
}

PinDecision sim_pin_decision(std::uint32_t refused, const std::string &pin,
                             int attempts_left)
{
    if (refused != 0 && refused == sim_pin_fingerprint(pin)) {
        return PinDecision::refused_before;
    }
    if (attempts_left >= 0 && attempts_left < kMinPinAttempts) {
        return PinDecision::too_few_attempts;
    }
    return PinDecision::send;
}

int parse_spic_pin_attempts(const std::string &line)
{
    const std::string prefix = "+SPIC:";
    const std::size_t at = line.find(prefix);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + prefix.size();
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    int value = -1;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        value = (value < 0 ? 0 : value * 10) + (line[i] - '0');
        if (value > 99) {
            return -1;
        }
        ++i;
    }
    return value;
}

}  // namespace hub_config
