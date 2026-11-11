#include "push_protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "uplink_protocol.hpp"

namespace uplink
{
namespace push
{

namespace
{

// Index just past the JSON value starting at `pos`; npos if broken.
std::size_t skip_value(const std::string &json, std::size_t pos)
{
    if (pos >= json.size()) {
        return std::string::npos;
    }
    const char first = json[pos];
    if (first == '"') {
        for (std::size_t i = pos + 1; i < json.size(); i++) {
            if (json[i] == '\\') {
                i++;
            } else if (json[i] == '"') {
                return i + 1;
            }
        }
        return std::string::npos;
    }
    if (first == '{' || first == '[') {
        int depth = 0;
        for (std::size_t i = pos; i < json.size(); i++) {
            const char c = json[i];
            if (c == '"') {
                i = skip_value(json, i);
                if (i == std::string::npos) {
                    return i;
                }
                i--;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) {
                    return i + 1;
                }
            }
        }
        return std::string::npos;
    }
    std::size_t i = pos;
    while (i < json.size() && json[i] != ',' && json[i] != '}' &&
           json[i] != ']' && json[i] != ' ' && json[i] != '\n' &&
           json[i] != '\r' && json[i] != '\t') {
        i++;
    }
    return i;
}

std::size_t skip_space(const std::string &json, std::size_t i)
{
    while (i < json.size() && (json[i] == ' ' || json[i] == '\n' ||
                               json[i] == '\r' || json[i] == '\t')) {
        i++;
    }
    return i;
}

std::int64_t to_int(const std::string &raw)
{
    return raw.empty() ? 0 : std::strtoll(raw.c_str(), nullptr, 10);
}

// A JSON string with its quotes.
std::string json_string(const std::string &text)
{
    return "\"" + json_escape(text) + "\"";
}

}  // namespace

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

std::string json_value(const std::string &object, const std::string &key)
{
    std::size_t i = skip_space(object, 0);
    if (i >= object.size() || object[i] != '{') {
        return "";
    }
    i++;
    while (true) {
        i = skip_space(object, i);
        if (i >= object.size() || object[i] == '}') {
            return "";
        }
        if (object[i] != '"') {
            return "";
        }
        const std::size_t key_end = skip_value(object, i);
        if (key_end == std::string::npos) {
            return "";
        }
        const std::string name = object.substr(i + 1, key_end - i - 2);
        i = skip_space(object, key_end);
        if (i >= object.size() || object[i] != ':') {
            return "";
        }
        i = skip_space(object, i + 1);
        const std::size_t value_end = skip_value(object, i);
        if (value_end == std::string::npos) {
            return "";
        }
        if (name == key) {
            return object.substr(i, value_end - i);
        }
        i = skip_space(object, value_end);
        if (i < object.size() && object[i] == ',') {
            i++;
        }
    }
}

std::string json_unquote(const std::string &raw)
{
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return raw;
    }
    std::string out;
    for (std::size_t i = 1; i + 1 < raw.size(); i++) {
        const char c = raw[i];
        if (c != '\\' || i + 2 >= raw.size()) {
            out += c;
            continue;
        }
        const char next = raw[++i];
        switch (next) {
        case 'n':
            out += '\n';
            break;
        case '"':
        case '\\':
        case '/':
            out += next;
            break;
        case 'u':
            if (i + 4 < raw.size()) {
                const long code =
                    std::strtol(raw.substr(i + 1, 4).c_str(), nullptr, 16);
                if (code > 0 && code < 0x80) {
                    out += (char)code;
                    i += 4;
                    break;
                }
            }
            out += "\\u";
            break;
        default:
            out += '\\';
            out += next;
        }
    }
    return out;
}

bool api_ok(const std::string &json)
{
    return json_value(json, "ok") == "true";
}

bool parse_updates(const std::string &json, std::vector<Update> &out)
{
    out.clear();
    if (!api_ok(json)) {
        return false;
    }
    const std::string result = json_value(json, "result");
    if (result.empty() || result[0] != '[') {
        return false;
    }
    std::size_t i = 1;
    while (true) {
        i = skip_space(result, i);
        if (i >= result.size() || result[i] == ']') {
            return true;
        }
        const std::size_t end = skip_value(result, i);
        if (end == std::string::npos) {
            return false;
        }
        const std::string item = result.substr(i, end - i);
        Update update;
        update.update_id = to_int(json_value(item, "update_id"));
        const std::string message = json_value(item, "message");
        const std::string callback = json_value(item, "callback_query");
        if (!message.empty()) {
            update.chat_id =
                to_int(json_value(json_value(message, "chat"), "id"));
            update.text = json_unquote(json_value(message, "text"));
        } else if (!callback.empty()) {
            update.callback_id = json_unquote(json_value(callback, "id"));
            update.callback_data = json_unquote(json_value(callback, "data"));
            update.chat_id = to_int(json_value(
                json_value(json_value(callback, "message"), "chat"), "id"));
        }
        out.push_back(update);
        i = skip_space(result, end);
        if (i < result.size() && result[i] == ',') {
            i++;
        }
    }
}

std::string command_of(const std::string &text)
{
    if (text.empty() || text[0] != '/') {
        return "";
    }
    std::size_t end = 1;
    while (end < text.size() && text[end] != ' ' && text[end] != '@' &&
           text[end] != '\n') {
        end++;
    }
    return text.substr(0, end);
}

std::string video_button(const std::string &camera_id, std::int64_t media_id)
{
    std::string data = "v:" + std::to_string((long long)media_id) + ":";
    return data + camera_id.substr(0, 64 - data.size());
}

bool parse_video_button(const std::string &data, std::string &camera_id,
                        std::int64_t &media_id)
{
    if (data.rfind("v:", 0) != 0) {
        return false;
    }
    const std::size_t colon = data.find(':', 2);
    if (colon == std::string::npos || colon == 2 || colon + 1 >= data.size()) {
        return false;
    }
    for (std::size_t i = 2; i < colon; i++) {
        if (data[i] < '0' || data[i] > '9') {
            return false;
        }
    }
    media_id = std::strtoll(data.substr(2, colon - 2).c_str(), nullptr, 10);
    camera_id = data.substr(colon + 1);
    return true;
}

std::string inline_keyboard(const std::string &text, const std::string &data)
{
    return "{\"inline_keyboard\":[[{\"text\":" + json_string(text) +
           ",\"callback_data\":" + json_string(data) + "}]]}";
}

std::string
multipart_head(const std::string &boundary,
               const std::vector<std::pair<std::string, std::string>> &fields,
               const std::string &file_field, const std::string &file_name,
               const std::string &content_type)
{
    std::string head;
    for (const auto &field : fields) {
        head += "--" + boundary + "\r\n";
        head += "Content-Disposition: form-data; name=\"" + field.first +
                "\"\r\n\r\n";
        head += field.second + "\r\n";
    }
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"" + file_field +
            "\"; filename=\"" + file_name + "\"\r\n";
    head += "Content-Type: " + content_type + "\r\n\r\n";
    return head;
}

std::string multipart_tail(const std::string &boundary)
{
    return "\r\n--" + boundary + "--\r\n";
}

}  // namespace push
}  // namespace uplink
