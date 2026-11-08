#pragma once

// Whether and when the hub installs a firmware release. Pure logic,
// host-testable; the download and the flash writes live in ota_update.cpp.
//
// The hub reads the manifest of the latest GitHub release (ota.json, see
// docs/ota.md) itself, at most once a day.
//
// Only release builds update themselves: their version is a plain
// "vMAJOR.MINOR.PATCH" (the git tag the CI built them from). A development
// build (a commit hash, "v1.2.3-4-gabc", "-dirty", a pre-release) is never
// replaced automatically, so a test build on the bench stays as it is.
//
// An update also needs enough battery and, over the cellular modem, a usable
// signal; a version whose install failed too often waits a day.

#include <cstdint>
#include <string>

namespace ota
{

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

// "v1.2.3" or "1.2.3"; false for anything else.
bool parse_version(const std::string &text, Version &out);

// -1, 0, 1 like strcmp.
int compare(const Version &a, const Version &b);

// A firmware image of a release, from the manifest.
struct Offer {
    std::string version;
    std::string url;
    std::string sha256;  // lowercase hex
    std::int64_t size = 0;
};

// Reads the image `image_key` ("<board>-<variant>", e.g. "esp32s3-release")
// from a release manifest:
//
//   {"version":"v1.0.0","images":{"esp32s3-release":
//       {"url":"https://…","sha256":"…","size":1572864}}}
//
// False if the image is missing or a field is invalid (an https URL, 64 hex
// digits, a positive size).
bool parse_manifest(const std::string &json, const std::string &image_key,
                    Offer &out);

enum class Decision {
    install,
    disabled,          // automatic updates are off in this build
    dev_build,         // the running firmware is not a release
    invalid_offer,     // the offered version is not a release version
    not_newer,         // the offer is not newer than the running firmware
    rolled_back,       // this version was installed before and rolled back
    battery_critical,  // the hub battery is in the critical level
    battery_low,       // in the low level, or below the minimum and not
                       // charging
    weak_signal,       // the modem's signal is too weak
    gave_up,           // its install failed too often; tried again later
};

// What the hub knows about itself when it could update.
struct Conditions {
    // Hub battery: percentage (-1 unknown), charging, and the level of the
    // schedule (0 normal, 1 low, 2 critical; see schedule::PowerLevel).
    int battery_percent = -1;
    bool charging = false;
    int power_level = 0;
    // The sync runs over the cellular modem, with this signal (dBm, 0
    // unknown).
    bool over_modem = false;
    int signal_dbm = 0;
};

struct Policy {
    // Install only at or above this battery percentage, unless the cell is
    // being charged or the battery is unknown.
    int min_battery_percent = 40;
    // Over the modem: install only at or above this signal (dBm).
    int min_signal_dbm = -105;
    // After this many failed installs of one version, wait retry_after_s
    // before the next attempt.
    std::uint32_t max_attempts = 3;
    std::int64_t retry_after_s = 24 * 3600;
    // Read the manifest at most this often.
    std::int64_t check_interval_s = 24 * 3600;
};

// Failed installs of one version (stored in NVS).
struct Attempts {
    std::string version;
    std::uint32_t failures = 0;
    std::int64_t last_failure = 0;  // Unix seconds; 0 unknown
};

// Whether this hub may update now at all: checked before the manifest is
// read, so that a hub that would not install anything does not spend data
// on it.
Decision ready(bool automatic, const std::string &running,
               const Conditions &conditions, const Policy &policy);

// The manifest is due: never read, the interval has passed, or the clock is
// not set (`now` <= 0) or went back.
bool manifest_due(std::int64_t last_check, std::int64_t now,
                  const Policy &policy);

// The decision for an offered version (`rolled_back`: version of the
// firmware the bootloader rolled back from, empty if none). `now` is Unix
// time, 0 if the clock is not set (a version that failed too often then
// waits).
Decision decide(const std::string &offered, const std::string &running,
                const std::string &rolled_back, bool automatic,
                const Conditions &conditions, const Policy &policy,
                const Attempts &attempts, std::int64_t now);

// The version-only part of decide() (no battery, signal or attempts).
Decision decide(const std::string &offered, const std::string &running,
                const std::string &rolled_back, bool automatic);

// An install of `version` failed at `now`. A different version starts the
// count afresh.
void record_failure(Attempts &attempts, const std::string &version,
                    std::int64_t now);

const char *decision_text(Decision decision);

// Lowercase hex of a SHA-256 digest.
std::string to_hex(const std::uint8_t *digest, std::size_t length);

}  // namespace ota
