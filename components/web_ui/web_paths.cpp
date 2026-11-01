#include "web_paths.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace web
{

namespace
{

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool ends_with(const std::string &text, const char *suffix)
{
    const std::size_t length = std::strlen(suffix);
    if (text.size() < length)
        return false;
    for (std::size_t i = 0; i < length; i++) {
        const char c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(text[text.size() - length + i])));
        if (c != suffix[i])
            return false;
    }
    return true;
}

}  // namespace

bool url_decode(const std::string &in, std::string &out)
{
    out.clear();
    for (std::size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size())
                return false;
            const int high = hex_value(in[i + 1]);
            const int low = hex_value(in[i + 2]);
            if (high < 0 || low < 0)
                return false;
            c = static_cast<char>(high * 16 + low);
            if (c == '\0')
                return false;
            i += 2;
        }
        out.push_back(c);
    }
    return true;
}

bool sanitize_path(const std::string &in, std::string &out)
{
    out.clear();
    std::size_t start = 0;
    while (start <= in.size()) {
        std::size_t end = in.find('/', start);
        if (end == std::string::npos)
            end = in.size();
        const std::string segment = in.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".")
            continue;
        if (segment == "..")
            return false;
        for (char c : segment) {
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20)
                return false;
        }
        if (!out.empty())
            out.push_back('/');
        out += segment;
    }
    return true;
}

bool is_temporary_file(const std::string &name)
{
    return ends_with(name, ".part") || ends_with(name, ".tmp");
}

const char *content_type(const std::string &name)
{
    if (ends_with(name, ".jpg") || ends_with(name, ".jpeg"))
        return "image/jpeg";
    if (ends_with(name, ".mp4"))
        return "video/mp4";
    if (ends_with(name, ".mov"))
        return "video/quicktime";
    if (ends_with(name, ".avi"))
        return "video/x-msvideo";
    if (ends_with(name, ".aac"))
        return "audio/aac";
    if (ends_with(name, ".txt") || ends_with(name, ".csv") ||
        ends_with(name, ".log"))
        return "text/plain; charset=utf-8";
    if (ends_with(name, ".html"))
        return "text/html; charset=utf-8";
    if (ends_with(name, ".json"))
        return "application/json";
    return "application/octet-stream";
}

bool mp4_sources(const std::string &mp4_path, std::string &video,
                 std::string &audio)
{
    if (mp4_path.size() <= 4 || !ends_with(mp4_path, ".mp4")) {
        return false;
    }
    const std::string base = mp4_path.substr(0, mp4_path.size() - 4);
    video = base + ".h264";
    audio = base + ".aac";
    return true;
}

namespace
{
bool parse_number(const std::string &text, std::uint64_t &value)
{
    if (text.empty() || text.size() > 18) {
        return false;
    }
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}
}  // namespace

RangeResult parse_range(const std::string &header, std::uint64_t size,
                        std::uint64_t &first, std::uint64_t &last)
{
    static const std::string kUnit = "bytes=";
    if (header.compare(0, kUnit.size(), kUnit) != 0) {
        return RangeResult::kNone;
    }
    const std::string spec = header.substr(kUnit.size());
    const size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find(',') != std::string::npos) {
        return RangeResult::kNone;  // several ranges: send everything
    }
    const std::string from = spec.substr(0, dash);
    const std::string to = spec.substr(dash + 1);
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    if (from.empty()) {  // the last `b` bytes
        if (!parse_number(to, b)) {
            return RangeResult::kNone;
        }
        if (b == 0 || size == 0) {
            return RangeResult::kUnsatisfiable;
        }
        first = b >= size ? 0 : size - b;
        last = size - 1;
        return RangeResult::kPartial;
    }
    if (!parse_number(from, a)) {
        return RangeResult::kNone;
    }
    if (to.empty()) {
        b = size > 0 ? size - 1 : 0;
    } else if (!parse_number(to, b) || b < a) {
        return RangeResult::kNone;
    }
    if (a >= size) {
        return RangeResult::kUnsatisfiable;
    }
    first = a;
    last = b >= size ? size - 1 : b;
    return RangeResult::kPartial;
}

namespace
{
// Position of the value of `key`, or npos.
size_t json_value_at(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    size_t at = json.find(needle);
    while (at != std::string::npos) {
        size_t i = at + needle.size();
        while (i < json.size() && json[i] == ' ') {
            i++;
        }
        if (i < json.size() && json[i] == ':') {
            i++;
            while (i < json.size() && json[i] == ' ') {
                i++;
            }
            return i;
        }
        at = json.find(needle, at + 1);
    }
    return std::string::npos;
}
}  // namespace

bool json_int(const std::string &json, const std::string &key, long long &value)
{
    size_t i = json_value_at(json, key);
    if (i == std::string::npos) {
        return false;
    }
    bool negative = false;
    if (i < json.size() && json[i] == '-') {
        negative = true;
        i++;
    }
    if (i >= json.size() || json[i] < '0' || json[i] > '9') {
        return false;
    }
    long long result = 0;
    for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; i++) {
        result = result * 10 + (json[i] - '0');
    }
    value = negative ? -result : result;
    return true;
}

bool json_string(const std::string &json, const std::string &key,
                 std::string &value)
{
    size_t i = json_value_at(json, key);
    if (i == std::string::npos || i >= json.size() || json[i] != '"') {
        return false;
    }
    value.clear();
    for (i++; i < json.size(); i++) {
        if (json[i] == '"') {
            return true;
        }
        if (json[i] == '\\' && i + 1 < json.size()) {
            i++;
        }
        value.push_back(json[i]);
    }
    return false;
}

void append_json_string(std::string &out, const std::string &text)
{
    out.push_back('"');
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", u);
            out += escape;
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

}  // namespace web
