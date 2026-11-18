#include "push_protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "uplink_protocol.hpp"

namespace uplink
{
namespace push
{

std::string file_name(std::int64_t media_time, std::int64_t media_id,
                      const char *extension)
{
    std::string name = std::to_string((long long)media_id) + "." + extension;
    if (media_time <= 0) {
        return name;
    }
    const std::time_t t = (std::time_t)media_time;
    std::tm tm = {};
    gmtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S_", &tm);
    return stamp + name;
}

std::string wall_time(std::int64_t media_time)
{
    if (media_time <= 0) {
        return "";
    }
    const std::time_t t = (std::time_t)media_time;
    std::tm tm = {};
    gmtime_r(&t, &tm);
    char text[24];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
    return text;
}

std::string encode_segment(const std::string &segment)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : segment) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        }
    }
    return out;
}

std::string join_url(const std::string &base,
                     const std::vector<std::string> &segments)
{
    std::string url = base;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    for (const std::string &segment : segments) {
        url += "/" + encode_segment(segment);
    }
    return url;
}

}  // namespace push
}  // namespace uplink
