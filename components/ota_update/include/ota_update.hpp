#pragma once

// Firmware updates over the backend check-in (see docs/ota.md). The backend
// offers the newest GitHub release for the hub's board and build variant;
// the hub downloads it over HTTPS into the other OTA slot, checks size,
// SHA-256 and the image's own description, and restarts into it. The new
// firmware confirms itself after its first successful check-in; if it
// restarts before (crash, watchdog, deep sleep), the bootloader goes back to
// the old one (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE), and that version is
// not installed again automatically.

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace ota
{

struct Offer {
    std::string version;
    std::string url;
    std::string sha256;  // lowercase hex
    std::int64_t size = 0;
};

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

// Downloads, checks and activates the offered firmware. On success the next
// restart boots it (see restart()). `error` explains a failure.
esp_err_t install(const Offer &offer, std::string &error);

// Writes the SD log and restarts.
[[noreturn]] void restart();

}  // namespace ota
