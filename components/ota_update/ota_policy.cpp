#include "ota_policy.hpp"

#include <cstddef>

namespace ota
{

namespace
{

// Parses digits at `pos` up to a '.' or the end; no leading zeros.
bool number(const std::string &text, std::size_t &pos, std::uint32_t &out)
{
    const std::size_t start = pos;
    std::uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (std::uint64_t)(text[pos] - '0');
        if (value > 0xFFFFFFFFu) {
            return false;
        }
        ++pos;
    }
    const std::size_t digits = pos - start;
    if (digits == 0 || (digits > 1 && text[start] == '0')) {
        return false;
    }
    out = (std::uint32_t)value;
    return true;
}

}  // namespace

bool parse_version(const std::string &text, Version &out)
{
    std::size_t pos = !text.empty() && text[0] == 'v' ? 1 : 0;
    Version v;
    if (!number(text, pos, v.major) || pos >= text.size() || text[pos] != '.') {
        return false;
    }
    ++pos;
    if (!number(text, pos, v.minor) || pos >= text.size() || text[pos] != '.') {
        return false;
    }
    ++pos;
    if (!number(text, pos, v.patch) || pos != text.size()) {
        return false;
    }
    out = v;
    return true;
}

int compare(const Version &a, const Version &b)
{
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

Decision decide(const std::string &offered, const std::string &running,
                const std::string &rolled_back, bool automatic)
{
    if (!automatic) {
        return Decision::disabled;
    }
    Version current;
    if (!parse_version(running, current)) {
        return Decision::dev_build;
    }
    Version offer;
    if (!parse_version(offered, offer)) {
        return Decision::invalid_offer;
    }
    if (compare(offer, current) <= 0) {
        return Decision::not_newer;
    }
    Version bad;
    if (parse_version(rolled_back, bad) && compare(offer, bad) == 0) {
        return Decision::rolled_back;
    }
    return Decision::install;
}

const char *decision_text(Decision decision)
{
    switch (decision) {
    case Decision::install:
        return "install";
    case Decision::disabled:
        return "automatic updates are off";
    case Decision::dev_build:
        return "a development build is not updated automatically";
    case Decision::invalid_offer:
        return "the offered version is not a release";
    case Decision::not_newer:
        return "not newer than the running firmware";
    case Decision::rolled_back:
        return "this version was rolled back before";
    }
    return "";
}

std::string to_hex(const std::uint8_t *digest, std::size_t length)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[digest[i] >> 4]);
        out.push_back(kDigits[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace ota
