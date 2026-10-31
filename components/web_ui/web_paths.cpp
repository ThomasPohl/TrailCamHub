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
