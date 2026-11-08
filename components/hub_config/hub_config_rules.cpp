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
    case Key::power:
        return "power";
    case Key::direction:
        return "direction";
    case Key::destination:
        return "destination";
    case Key::webdav_url:
        return "webdav_url";
    case Key::webdav_user:
        return "webdav_user";
    case Key::webdav_password:
        return "webdav_password";
    case Key::telegram_token:
        return "telegram_token";
    case Key::telegram_chat:
        return "telegram_chat";
    case Key::health_url:
        return "health_url";
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
           key == Key::sim_pin || key == Key::webdav_password ||
           key == Key::telegram_token;
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
    case Key::power:
        return settings.power;
    case Key::direction:
        return settings.direction;
    case Key::destination:
        return settings.destination;
    case Key::webdav_url:
        return settings.webdav_url;
    case Key::webdav_user:
        return settings.webdav_user;
    case Key::webdav_password:
        return settings.webdav_password;
    case Key::telegram_token:
        return settings.telegram_token;
    case Key::telegram_chat:
        return settings.telegram_chat;
    case Key::health_url:
        return settings.health_url;
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
        if (value != "wifi" && value != "modem" && value != "ethernet") {
            return "transport must be wifi, ethernet or modem";
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
    case Key::power:
        return value == "mains" || value == "battery"
                   ? ""
                   : "power must be mains or battery";
    case Key::direction:
        return value == "push" || value == "pull"
                   ? ""
                   : "direction must be push or pull";
    case Key::destination:
        return value == "backend" || value == "webdav" || value == "telegram"
                   ? ""
                   : "destination must be backend, webdav or telegram";
    case Key::webdav_url:
        if ((value.rfind("https://", 0) != 0 &&
             value.rfind("http://", 0) != 0) ||
            value.size() <= 8) {
            return "WebDAV URL must start with https:// or http://";
        }
        if (value.size() > 200 || has_control_chars(value) ||
            value.find(' ') != std::string::npos) {
            return "WebDAV URL is too long or contains invalid characters";
        }
        return "";
    case Key::webdav_user:
        if (value.size() > 64 || has_control_chars(value) ||
            value.find(':') != std::string::npos) {
            return "WebDAV user must be up to 64 characters, without ':'";
        }
        return "";
    case Key::webdav_password:
        if (value.size() > 128 || has_control_chars(value)) {
            return "WebDAV password must be up to 128 characters";
        }
        return "";
    case Key::telegram_token: {
        // <bot id>:<secret>, e.g. 123456789:AAH...
        const std::size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon > 20 ||
            value.size() - colon - 1 < 30 || value.size() > 100) {
            return "bot token must look like 123456789:AA... (from "
                   "@BotFather)";
        }
        for (std::size_t i = 0; i < value.size(); i++) {
            const char c = value[i];
            if (i < colon ? (c < '0' || c > '9')
                          : (i != colon && !is_key_char(c))) {
                return "bot token contains invalid characters";
            }
        }
        return "";
    }
    case Key::health_url:
        if ((value.rfind("https://", 0) != 0 &&
             value.rfind("http://", 0) != 0) ||
            value.size() <= 8 || value.size() > 200 ||
            has_control_chars(value) || value.find(' ') != std::string::npos) {
            return "health check URL must start with https:// or http://";
        }
        return "";
    case Key::telegram_chat: {
        // A chat id: a user (positive) or a group (negative).
        std::size_t i = value.size() > 1 && value[0] == '-' ? 1 : 0;
        if (value.size() - i < 1 || value.size() > 20) {
            return "Telegram chat must be a chat id (digits, - for groups)";
        }
        for (; i < value.size(); i++) {
            if (value[i] < '0' || value[i] > '9') {
                return "Telegram chat must be a chat id (digits, - for "
                       "groups)";
            }
        }
        return "";
    }
    }
    return "unknown setting";
}

std::string display_value(Key key, const std::string &value)
{
    if (value.empty()) {
        return key == Key::camera_plan || key == Key::checkin_min ||
                       key == Key::transport || key == Key::apn ||
                       key == Key::power || key == Key::direction ||
                       key == Key::destination || key == Key::telegram_chat
                   ? "(default)"
                   : "(not set)";
    }
    if (key == Key::wifi_password || key == Key::sim_pin ||
        key == Key::webdav_password) {
        return "(set)";
    }
    if (key == Key::telegram_token) {
        // The bot id is not secret and tells two bots apart.
        return value.substr(0, value.find(':')) + ":*** (set)";
    }
    if (key == Key::device_key) {
        const std::size_t dot = value.find('.');
        return value.substr(0, dot) + ".*** (set)";
    }
    return value;
}

Transport transport(const Settings &settings, const Capabilities &caps)
{
    if (settings.transport == "ethernet" && caps.ethernet) {
        return Transport::ethernet;
    }
    if (settings.transport == "modem" && caps.modem) {
        return Transport::modem;
    }
    if (settings.transport == "wifi") {
        return Transport::wifi;
    }
    if (caps.ethernet) {
        return Transport::ethernet;
    }
    if (!settings.wifi_ssid.empty() || !caps.modem) {
        return Transport::wifi;
    }
    return Transport::modem;
}

Destination destination(const Settings &settings)
{
    if (settings.destination == "webdav") {
        return Destination::webdav;
    }
    if (settings.destination == "telegram") {
        return Destination::telegram;
    }
    return Destination::backend;
}

bool destination_configured(const Settings &settings)
{
    switch (destination(settings)) {
    case Destination::webdav:
        return validate(Key::webdav_url, settings.webdav_url).empty();
    case Destination::telegram:
        return validate(Key::telegram_token, settings.telegram_token).empty();
    case Destination::backend:
        break;
    }
    return validate(Key::api_url, settings.api_url).empty() &&
           validate(Key::device_key, settings.device_key).empty();
}

bool uplink_configured(const Settings &settings, const Capabilities &caps)
{
    if (!destination_configured(settings)) {
        return false;
    }
    if (transport(settings, caps) != Transport::wifi) {
        return true;
    }
    return validate(Key::wifi_ssid, settings.wifi_ssid).empty() &&
           validate(Key::wifi_password, settings.wifi_password).empty();
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
    switch (transport) {
    case Transport::modem:
        return "modem";
    case Transport::ethernet:
        return "ethernet";
    case Transport::wifi:
        break;
    }
    return "wifi";
}

bool uplink_configured(const Settings &settings, bool modem_available)
{
    if (!destination_configured(settings)) {
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

Mode resolve_mode(const Settings &settings, const Capabilities &caps,
                  const Preset &preset)
{
    Mode mode;
    mode.power = preset.power;
    if (settings.power == "mains" || settings.power == "battery") {
        mode.power =
            settings.power == "battery" ? Power::battery : Power::mains;
        mode.power_origin = ModeOrigin::setting;
    }
    mode.direction = preset.direction;
    if (settings.direction == "push") {
        mode.direction = Direction::push;
        mode.direction_origin = ModeOrigin::setting;
    } else if (settings.direction == "pull" && caps.web_ui) {
        mode.direction = Direction::pull;
        mode.direction_origin = ModeOrigin::setting;
    }
    if (mode.direction == Direction::pull && !caps.web_ui) {
        // Nothing to pull from.
        mode.direction = Direction::push;
    }
    mode.transport = transport(settings, caps);
    mode.destination = destination(settings);
    return mode;
}

const char *power_name(Power power)
{
    return power == Power::battery ? "battery" : "mains";
}

const char *direction_name(Direction direction)
{
    return direction == Direction::pull ? "pull" : "push";
}

const char *destination_name(Destination destination)
{
    switch (destination) {
    case Destination::webdav:
        return "webdav";
    case Destination::telegram:
        return "telegram";
    case Destination::backend:
        break;
    }
    return "backend";
}

const char *origin_name(ModeOrigin origin)
{
    return origin == ModeOrigin::setting ? "setting" : "preset";
}

bool sleeps(const Mode &mode, const Capabilities &caps)
{
    return caps.deep_sleep && mode.power == Power::battery;
}

bool wifi_power_save(const Mode &mode)
{
    return mode.power == Power::battery;
}

bool reads_on_schedule(const Mode &mode, bool uplink)
{
    // On mains without a destination someone is on site (button, web UI)
    // and starts the reads.
    return mode.power == Power::battery ||
           (mode.direction == Direction::push && uplink);
}

bool checks_in(const Mode &mode)
{
    return mode.direction == Direction::push;
}

std::string capability_problem(Key key, const std::string &value,
                               const Capabilities &caps)
{
    if (key == Key::transport && value == "modem" && !caps.modem) {
        return "this board has no modem";
    }
    if (key == Key::transport && value == "ethernet" && !caps.ethernet) {
        return "this board has no Ethernet";
    }
    // Decision in #150: Telegram directly only where nothing else reaches
    // the hub (cellular modem); at home WebDAV or the web UI.
    if (key == Key::destination && value == "telegram" && !caps.modem) {
        return "Telegram directly needs a board with a modem; at home use "
               "webdav or the web UI";
    }
    if (key == Key::direction && value == "pull" && !caps.web_ui) {
        return "pull needs the web UI, which this build does not have";
    }
    if (key == Key::power && value == "battery" && !caps.deep_sleep) {
        return "this build has no deep sleep for battery power";
    }
    return "";
}

}  // namespace hub_config
