#include "artemis_json.hpp"

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

std::string json_set_int(const std::string &json, const std::string &key,
                         std::int64_t value)
{
    const std::string field = "\"" + key + "\":" + std::to_string(value);
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at != std::string::npos) {
        size_t colon = json.find(':', at + needle.size());
        if (colon != std::string::npos) {
            size_t end = colon + 1;
            while (end < json.size() &&
                   (json[end] == ' ' || json[end] == '-' ||
                    (json[end] >= '0' && json[end] <= '9'))) {
                end++;
            }
            return json.substr(0, at) + field + json.substr(end);
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

}  // namespace camera::artemis
