#pragma once

// Whether the hub installs a firmware the backend offers. Pure logic,
// host-testable; the download and the flash writes live in ota_update.cpp.
//
// Only release builds update themselves: their version is a plain
// "vMAJOR.MINOR.PATCH" (the git tag the CI built them from). A development
// build (a commit hash, "v1.2.3-4-gabc", "-dirty", a pre-release) is never
// replaced automatically, so a test build on the bench stays as it is.

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

enum class Decision {
    install,
    disabled,       // automatic updates are off in this build
    dev_build,      // the running firmware is not a release
    invalid_offer,  // the offered version is not a release version
    not_newer,      // the offer is not newer than the running firmware
    rolled_back,    // this version was installed before and rolled back
};

// `rolled_back`: version of the firmware the bootloader rolled back from
// (empty if none).
Decision decide(const std::string &offered, const std::string &running,
                const std::string &rolled_back, bool automatic);

const char *decision_text(Decision decision);

// Lowercase hex of a SHA-256 digest.
std::string to_hex(const std::uint8_t *digest, std::size_t length);

}  // namespace ota
