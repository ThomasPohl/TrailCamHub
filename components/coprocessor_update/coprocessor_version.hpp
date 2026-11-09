#pragma once

// Version numbers of the ESP-Hosted firmware on the ESP32-C6 (#62). Pure
// logic, host-tested: the version string in the embedded image's descriptor
// may carry a "v" prefix or a git describe suffix, while the C6 reports
// three numbers.

#include <cstdint>
#include <string>

namespace coprocessor
{

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

// "2.6.1", "v2.6.1", "2.6.1-3-gabc1234", "2.6.1-dirty", "ESP-Hosted 2.6.1":
// the first "<major>.<minor>.<patch>" in the text. False if there is none.
bool parse_version(const std::string &text, Version &out);

bool operator==(const Version &a, const Version &b);
inline bool operator!=(const Version &a, const Version &b)
{
    return !(a == b);
}

std::string to_string(const Version &version);

}  // namespace coprocessor
