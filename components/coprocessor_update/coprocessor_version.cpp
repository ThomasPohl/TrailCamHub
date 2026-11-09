#include "coprocessor_version.hpp"

namespace coprocessor
{

namespace
{

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// Reads a number at `i`; false if there is no digit or it is too large.
bool read_number(const std::string &text, std::size_t &i, std::uint32_t &out)
{
    if (i >= text.size() || !is_digit(text[i])) {
        return false;
    }
    std::uint64_t value = 0;
    while (i < text.size() && is_digit(text[i])) {
        value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
        if (value > 0xffffffffull) {
            return false;
        }
        ++i;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

bool parse_version(const std::string &text, Version &out)
{
    for (std::size_t start = 0; start < text.size(); ++start) {
        // A number that starts here, not in the middle of one.
        if (!is_digit(text[start]) ||
            (start > 0 && is_digit(text[start - 1]))) {
            continue;
        }
        std::size_t i = start;
        Version version;
        if (read_number(text, i, version.major) && i < text.size() &&
            text[i] == '.' && read_number(text, ++i, version.minor) &&
            i < text.size() && text[i] == '.' &&
            read_number(text, ++i, version.patch)) {
            out = version;
            return true;
        }
    }
    return false;
}

bool operator==(const Version &a, const Version &b)
{
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
}

std::string to_string(const Version &version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor) +
           "." + std::to_string(version.patch);
}

}  // namespace coprocessor
