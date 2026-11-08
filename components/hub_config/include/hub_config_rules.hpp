#pragma once

// Names, validation and display rules for the hub's stored settings (home
// Wi-Fi, cellular modem, backend access and the wake-up schedule). Pure logic,
// host-testable; storage in NVS lives in hub_config.cpp.

#include <cstddef>
#include <cstdint>
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
    // How the hub reaches its destination: "wifi", "ethernet" or "modem";
    // empty: Ethernet on a board that has it, else the home Wi-Fi if one is
    // set, otherwise the modem (see transport()).
    transport,
    // APN of the SIM card's provider for the modem; empty: the one the
    // network suggests.
    apn,
    // PIN of the SIM card (4 digits, all the modem driver sends); empty: the
    // card has no PIN.
    sim_pin,
    // Mode (docs/modes.md); empty: the board's preset.
    // "mains" or "battery": deep sleep, power save and the default schedule.
    power,
    // "push" (the hub sends to its destination) or "pull" (people fetch
    // from the hub's web UI).
    direction,
    // Where push sends to: "backend", "webdav" or "telegram" (docs/modes.md).
    destination,
    // WebDAV destination: folder URL (http:// or https://), user, password.
    webdav_url,
    webdav_user,
    webdav_password,
    // Telegram destination: the bot token from @BotFather, and the one chat
    // it talks to (set by /start; empty: the next chat that sends /start).
    telegram_token,
    telegram_chat,
    // Called after every sync (a health check service such as
    // healthchecks.io: "<url>" after success, "<url>/fail" after a
    // failure), so that it warns when the hub stops reporting.
    health_url,
};

constexpr Key kAllKeys[] = {
    Key::wifi_ssid,      Key::wifi_password, Key::api_url,
    Key::device_key,     Key::camera_plan,   Key::checkin_min,
    Key::transport,      Key::apn,           Key::sim_pin,
    Key::power,          Key::direction,     Key::destination,
    Key::webdav_url,     Key::webdav_user,   Key::webdav_password,
    Key::telegram_token, Key::telegram_chat};

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
    std::string power;
    std::string direction;
    std::string destination;
    std::string webdav_url;
    std::string webdav_user;
    std::string webdav_password;
    std::string telegram_token;
    std::string telegram_chat;
    std::string health_url;
};

enum class Transport {
    wifi,      // the home Wi-Fi
    modem,     // the cellular modem (PPP)
    ethernet,  // wired Ethernet (P4, W5500)
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

// --- Mode (#150) ---
//
// The hardware of a board is fixed (its capabilities, from Kconfig); how it
// is used is chosen at run time and stored in NVS. Each setting left empty
// takes the board's preset (the former profiles of #86).

enum class Power { mains, battery };
enum class Direction { push, pull };
enum class Destination { backend, webdav, telegram };

// What the build of this board can do.
struct Capabilities {
    bool modem = false;          // cellular modem (CONFIG_MODEM_UPLINK)
    bool ethernet = false;       // wired Ethernet
    bool battery_gauge = false;  // MAX17048
    bool psram = false;
    bool display = false;
    bool button = false;
    bool web_ui = false;      // pull needs it
    bool deep_sleep = false;  // battery power uses it
};

// The board's defaults for the settings left empty.
struct Preset {
    Power power = Power::mains;
    Direction direction = Direction::pull;
};

enum class ModeOrigin { preset, setting };

struct Mode {
    Power power = Power::mains;
    Direction direction = Direction::pull;
    Transport transport = Transport::wifi;
    Destination destination = Destination::backend;
    ModeOrigin power_origin = ModeOrigin::preset;
    ModeOrigin direction_origin = ModeOrigin::preset;
};

// The mode from the stored settings and the board. A setting the board
// cannot use (pull without a web UI) falls back to the preset.
Mode resolve_mode(const Settings &settings, const Capabilities &caps,
                  const Preset &preset);

const char *power_name(Power power);
const char *direction_name(Direction direction);
const char *destination_name(Destination destination);
const char *origin_name(ModeOrigin origin);

// What follows from the mode:
// deep sleep between cycles,
bool sleeps(const Mode &mode, const Capabilities &caps);
// modem sleep on the home Wi-Fi (except during large transfers),
bool wifi_power_save(const Mode &mode);
// the build's camera read plan when none is set: on battery, or when the
// hub pushes to a configured destination (`uplink`); otherwise no automatic
// reads, someone on site starts them,
bool reads_on_schedule(const Mode &mode, bool uplink);
// check-ins with the destination.
bool checks_in(const Mode &mode);

// How the hub reaches its destination on a board with `caps`: a stored
// transport the board has, else Ethernet if it has it, else the home Wi-Fi
// if one is set, else the modem if it has one, else the Wi-Fi.
Transport transport(const Settings &settings, const Capabilities &caps);

// True if the hub has everything it needs to reach its destination over the
// transport that applies on a board with `caps`.
bool uplink_configured(const Settings &settings, const Capabilities &caps);

// The destination's own settings are complete (backend URL and key, WebDAV
// URL, Telegram token).
bool destination_configured(const Settings &settings);
Destination destination(const Settings &settings);

// Longest check-in interval with Telegram directly: Telegram keeps updates
// (button presses, commands) for about a day.
constexpr std::uint32_t kTelegramMaxCheckinMin = 12 * 60;

// Empty if `value` for `key` fits the board, else a short reason (e.g.
// "this board has no modem"). For keys without such limits always empty.
std::string capability_problem(Key key, const std::string &value,
                               const Capabilities &caps);

// SIM PIN guard (#152). A PIN the SIM card refused is remembered (as this
// fingerprint, never 0) across cycles and deep sleep, so that the hub does
// not lock the card by sending it again in the next cycles; "config set
// sim_pin" forgets it.
std::uint32_t sim_pin_fingerprint(const std::string &pin);

enum class PinDecision {
    send,              // enter the PIN
    refused_before,    // the card refused exactly this PIN before
    too_few_attempts,  // fewer than kMinPinAttempts left: needs a phone
};

// Attempts that must be left before the hub enters a PIN: with one left, a
// wrong PIN would lock the card (PUK).
constexpr int kMinPinAttempts = 2;

// `refused`: the stored fingerprint (0: none). `attempts_left`: PIN attempts
// the card has left, -1 if unknown.
PinDecision sim_pin_decision(std::uint32_t refused, const std::string &pin,
                             int attempts_left);

// PIN attempts left from a "+SPIC: <pin1>,<puk1>,<pin2>,<puk2>" line of a
// SIMCom modem (A76xx); -1 if the line is not one.
int parse_spic_pin_attempts(const std::string &line);

}  // namespace hub_config
