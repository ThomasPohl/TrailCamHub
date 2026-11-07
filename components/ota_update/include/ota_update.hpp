#pragma once

// Firmware updates straight from the GitHub releases (see docs/ota.md). After
// a successful sync the hub reads the release manifest (ota.json, at most
// once a day), and if it offers a newer release for the hub's board and build
// variant and the conditions allow it (ota_policy.hpp: battery, modem signal,
// failed attempts), downloads the image over HTTPS into the other OTA slot,
// checks size, SHA-256 and the image's own description, and asks for a
// restart. The scheduler restarts the hub once the cycle is over and the
// modem is down.
//
// The new firmware confirms itself after its first successful check-in; if
// it restarts before (crash, watchdog, deep sleep), the bootloader goes back
// to the old one (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE), and that version is
// not installed again automatically.

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "ota_policy.hpp"

namespace ota
{

// Board and build variant of this firmware ("esp32s3", "release").
const char *board();
const char *variant();
// Version of the running firmware (esp_app_desc).
std::string running_version();
// Version of the firmware the bootloader rolled back from; empty if none.
std::string rolled_back_version();
// True if automatic updates are on in this build.
bool automatic();

// Logs the slot, the version and a pending verification or a rollback. Call
// once at boot.
void log_boot_state();

// The running firmware works (a check-in succeeded): cancels the rollback of
// a new firmware. Cheap to call again.
void confirm_running();

// Looks for a newer release and installs it if `conditions` allow (call it
// at the end of a successful sync). On success restart_pending() is set.
void check_for_update(const Conditions &conditions);

// The next check_for_update() reads the manifest even if it was read less
// than a day ago (console command "update").
void force_check();

// Downloads, checks and activates the offered firmware. A broken download is
// continued with HTTP range requests (up to three times). On success the
// next restart boots it. `error` explains a failure.
esp_err_t install(const Offer &offer, std::string &error);

// A new firmware is installed and waits for the restart.
bool restart_pending();

// One line about the last update check, for the console's status.
std::string last_check_text();

// Writes the SD log and restarts.
[[noreturn]] void restart();

}  // namespace ota
