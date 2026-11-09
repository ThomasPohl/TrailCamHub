#include "artemis_json.hpp"

#include <algorithm>
#include <cctype>

namespace camera::artemis
{

bool json_find_int(const std::string &json, const std::string &key,
                   std::int64_t &out, std::size_t from)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle, from);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != ':')
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    bool negative = false;
    if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) {
        negative = json[pos] == '-';
        ++pos;
    }
    if (pos >= json.size() ||
        !std::isdigit(static_cast<unsigned char>(json[pos]))) {
        return false;
    }
    std::int64_t value = 0;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

bool json_find_string(const std::string &json, const std::string &key,
                      std::string &out, std::size_t from)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle, from);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != ':')
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return false;
    ++pos;  // opening quote

    std::string value;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') {
            out = value;
            return true;
        }
        if (c == '\\' && pos < json.size()) {
            const char e = json[pos++];
            switch (e) {
            case 'n':
                value.push_back('\n');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            default:
                value.push_back(e);
                break;  // \" \\ \/ and others
            }
        } else {
            value.push_back(c);
        }
    }
    return false;  // unterminated string
}

namespace
{

// End of the value that starts after the colon at `colon`: a string literal
// or a (signed) integer.
size_t value_end(const std::string &json, size_t colon)
{
    size_t end = colon + 1;
    while (end < json.size() && json[end] == ' ') {
        end++;
    }
    if (end < json.size() && json[end] == '"') {
        end++;
        while (end < json.size() && json[end] != '"') {
            end += json[end] == '\\' ? 2 : 1;
        }
        return end < json.size() ? end + 1 : json.size();
    }
    while (end < json.size() &&
           (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) {
        end++;
    }
    return end;
}

std::string set_field(const std::string &json, const std::string &key,
                      const std::string &field)
{
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at != std::string::npos) {
        size_t colon = json.find(':', at + needle.size());
        if (colon != std::string::npos) {
            return json.substr(0, at) + field +
                   json.substr(value_end(json, colon));
        }
    }
    const size_t close = json.rfind('}');
    if (close == std::string::npos) {
        return json;
    }
    const bool empty = json.find_first_not_of(" \t\r\n{") == close;
    return json.substr(0, close) + (empty ? "" : ",") + field +
           json.substr(close);
}

}  // namespace

std::string json_set_int(const std::string &json, const std::string &key,
                         std::int64_t value)
{
    return set_field(json, key, "\"" + key + "\":" + std::to_string(value));
}

std::string json_quote(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            out.push_back(c);
        }
    }
    return out + "\"";
}

std::string json_set_string(const std::string &json, const std::string &key,
                            const std::string &value)
{
    return set_field(json, key, "\"" + key + "\":" + json_quote(value));
}

bool json_is_secret_key(const std::string &key)
{
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (lower.rfind("securecode", 0) == 0) {
        return lower != "securecodeonoff";
    }
    return lower.find("password") != std::string::npos ||
           lower.find("pwd") != std::string::npos;
}

namespace
{

// Index just past the string literal that starts with the quote at `pos`, or
// json.size() if it is not terminated.
std::size_t skip_string(const std::string &json, std::size_t pos)
{
    for (++pos; pos < json.size(); ++pos) {
        if (json[pos] == '\\') {
            ++pos;
        } else if (json[pos] == '"') {
            return pos + 1;
        }
    }
    return json.size();
}

// Index just past the JSON value that starts at `pos` (after whitespace).
std::size_t skip_value(const std::string &json, std::size_t pos)
{
    if (pos >= json.size()) {
        return pos;
    }
    if (json[pos] == '"') {
        return skip_string(json, pos);
    }
    if (json[pos] == '{' || json[pos] == '[') {
        int depth = 0;
        while (pos < json.size()) {
            const char c = json[pos];
            if (c == '"') {
                pos = skip_string(json, pos);
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    return pos + 1;
                }
            }
            ++pos;
        }
        return pos;
    }
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != ']' &&
           !std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return pos;
}

}  // namespace

std::string json_redact(const std::string &json)
{
    std::string out;
    out.reserve(json.size());
    std::size_t pos = 0;
    while (pos < json.size()) {
        if (json[pos] != '"') {
            out += json[pos++];
            continue;
        }
        const std::size_t end = skip_string(json, pos);
        out.append(json, pos, end - pos);
        std::size_t after = end;
        while (after < json.size() &&
               std::isspace(static_cast<unsigned char>(json[after]))) {
            ++after;
        }
        const bool is_key = after < json.size() && json[after] == ':';
        if (!is_key || end - pos < 2 ||
            !json_is_secret_key(json.substr(pos + 1, end - pos - 2))) {
            pos = end;
            continue;
        }
        // A secret key: copy up to and including ':' and any whitespace,
        // then replace the value.
        std::size_t value = after + 1;
        while (value < json.size() &&
               std::isspace(static_cast<unsigned char>(json[value]))) {
            ++value;
        }
        out.append(json, end, value - end);
        const std::size_t value_end = skip_value(json, value);
        if (value_end > value) {
            out += "\"***\"";
        }
        pos = value_end;
    }
    return out;
}

}  // namespace camera::artemis
