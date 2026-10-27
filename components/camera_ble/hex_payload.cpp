#include "hex_payload.hpp"

#include <cctype>

namespace camera
{

namespace
{

int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    const char lower =
        static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (lower >= 'a' && lower <= 'f') {
        return lower - 'a' + 10;
    }
    return -1;
}

bool is_separator(char value)
{
    return std::isspace(static_cast<unsigned char>(value)) || value == ':' ||
           value == '-';
}

}  // namespace

bool parse_hex_payload(const char *text, std::uint8_t *out,
                       std::size_t capacity, std::size_t *out_len)
{
    if (text == nullptr || out_len == nullptr ||
        (out == nullptr && capacity > 0)) {
        return false;
    }
    std::size_t length = 0;
    const char *cursor = text;
    while (*cursor != '\0') {
        while (is_separator(*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const int high = hex_value(*cursor++);
        if (*cursor == '\0') {
            return false;
        }
        const int low = hex_value(*cursor++);
        if (high < 0 || low < 0 || length >= capacity) {
            return false;
        }
        out[length++] = static_cast<std::uint8_t>((high << 4) | low);
    }
    *out_len = length;
    return true;
}

}  // namespace camera
