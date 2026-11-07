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

// Index just past the string starting at `pos` (which holds '"').
std::size_t skip_string(const std::string &json, std::size_t pos)
{
    for (std::size_t i = pos + 1; i < json.size(); i++) {
        if (json[i] == '\\') {
            i++;
        } else if (json[i] == '"') {
            return i + 1;
        }
    }
    return std::string::npos;
}

// Index of the value of the member `key` of the object [begin, end): the
// first non-blank character after its colon; npos if missing. Only members
// of that object count, not those of nested objects.
std::size_t member(const std::string &json, std::size_t begin, std::size_t end,
                   const std::string &key)
{
    int depth = 0;
    for (std::size_t i = begin; i < end;) {
        const char c = json[i];
        if (c == '"') {
            const std::size_t after = skip_string(json, i);
            if (after == std::string::npos || after > end) {
                return std::string::npos;
            }
            if (depth == 1 && json.compare(i + 1, after - i - 2, key) == 0 &&
                after - i - 2 == key.size()) {
                std::size_t value = json.find_first_not_of(" \t\r\n", after);
                if (value < end && json[value] == ':') {
                    value = json.find_first_not_of(" \t\r\n", value + 1);
                    return value < end ? value : std::string::npos;
                }
            }
            i = after;
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
        }
        i++;
    }
    return std::string::npos;
}

// End (one past the closing brace) of the object starting at `open`.
std::size_t object_end(const std::string &json, std::size_t open)
{
    int depth = 0;
    for (std::size_t i = open; i < json.size();) {
        const char c = json[i];
        if (c == '"') {
            i = skip_string(json, i);
            if (i == std::string::npos) {
                return std::string::npos;
            }
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) {
                return i + 1;
            }
        }
        i++;
    }
    return std::string::npos;
}

// A string value without escapes other than "\/".
bool string_at(const std::string &json, std::size_t pos, std::string &out)
{
    if (pos == std::string::npos || json[pos] != '"') {
        return false;
    }
    std::string value;
    for (std::size_t i = pos + 1; i < json.size(); i++) {
        const char c = json[i];
        if (c == '"') {
            out = value;
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= json.size() || json[i + 1] != '/') {
                return false;
            }
            i++;
            value.push_back('/');
            continue;
        }
        value.push_back(c);
    }
    return false;
}

bool positive_int_at(const std::string &json, std::size_t pos,
                     std::int64_t &out)
{
    if (pos == std::string::npos) {
        return false;
    }
    std::int64_t value = 0;
    std::size_t i = pos;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9' &&
           value < (1LL << 40)) {
        value = value * 10 + (json[i] - '0');
        i++;
    }
    if (i == pos || value <= 0 ||
        (i < json.size() && json[i] != ',' && json[i] != '}' &&
         json[i] != ' ' && json[i] != '\n' && json[i] != '\r' &&
         json[i] != '\t')) {
        return false;
    }
    out = value;
    return true;
}

bool is_sha256_hex(const std::string &text)
{
    if (text.size() != 64) {
        return false;
    }
    for (char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool parse_manifest(const std::string &json, const std::string &image_key,
                    Offer &out)
{
    const std::size_t open = json.find('{');
    if (open == std::string::npos) {
        return false;
    }
    const std::size_t end = object_end(json, open);
    if (end == std::string::npos) {
        return false;
    }
    Offer offer;
    if (!string_at(json, member(json, open, end, "version"), offer.version)) {
        return false;
    }
    const std::size_t images = member(json, open, end, "images");
    if (images == std::string::npos || json[images] != '{') {
        return false;
    }
    const std::size_t images_end = object_end(json, images);
    if (images_end == std::string::npos) {
        return false;
    }
    const std::size_t image = member(json, images, images_end, image_key);
    if (image == std::string::npos || json[image] != '{') {
        return false;
    }
    const std::size_t image_end = object_end(json, image);
    if (image_end == std::string::npos) {
        return false;
    }
    if (!string_at(json, member(json, image, image_end, "url"), offer.url) ||
        !string_at(json, member(json, image, image_end, "sha256"),
                   offer.sha256) ||
        !positive_int_at(json, member(json, image, image_end, "size"),
                         offer.size)) {
        return false;
    }
    if (offer.url.compare(0, 8, "https://") != 0 ||
        !is_sha256_hex(offer.sha256)) {
        return false;
    }
    for (char &c : offer.sha256) {
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
    }
    out = offer;
    return true;
}

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

Decision ready(bool automatic, const std::string &running,
               const Conditions &conditions, const Policy &policy)
{
    if (!automatic) {
        return Decision::disabled;
    }
    Version current;
    if (!parse_version(running, current)) {
        return Decision::dev_build;
    }
    if (conditions.power_level >= 2) {
        return Decision::battery_critical;
    }
    if (conditions.power_level == 1) {
        return Decision::battery_low;
    }
    if (conditions.battery_percent >= 0 && !conditions.charging &&
        conditions.battery_percent < policy.min_battery_percent) {
        return Decision::battery_low;
    }
    if (conditions.over_modem && conditions.signal_dbm != 0 &&
        conditions.signal_dbm < policy.min_signal_dbm) {
        return Decision::weak_signal;
    }
    return Decision::install;
}

bool manifest_due(std::int64_t last_check, std::int64_t now,
                  const Policy &policy)
{
    if (last_check <= 0 || now <= 0 || now < last_check) {
        return true;
    }
    return now - last_check >= policy.check_interval_s;
}

Decision decide(const std::string &offered, const std::string &running,
                const std::string &rolled_back, bool automatic,
                const Conditions &conditions, const Policy &policy,
                const Attempts &attempts, std::int64_t now)
{
    const Decision first = ready(automatic, running, conditions, policy);
    if (first != Decision::install) {
        return first;
    }
    const Decision version = decide(offered, running, rolled_back, automatic);
    if (version != Decision::install) {
        return version;
    }
    if (attempts.version == offered && policy.max_attempts > 0 &&
        attempts.failures >= policy.max_attempts) {
        const bool waited = now > 0 && attempts.last_failure > 0 &&
                            now >= attempts.last_failure &&
                            now - attempts.last_failure >= policy.retry_after_s;
        if (!waited) {
            return Decision::gave_up;
        }
    }
    return Decision::install;
}

void record_failure(Attempts &attempts, const std::string &version,
                    std::int64_t now)
{
    if (attempts.version != version) {
        attempts.version = version;
        attempts.failures = 0;
    }
    attempts.failures++;
    attempts.last_failure = now;
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
    case Decision::battery_critical:
        return "the hub battery is critical";
    case Decision::battery_low:
        return "the hub battery is too low";
    case Decision::weak_signal:
        return "the cellular signal is too weak";
    case Decision::gave_up:
        return "its install failed too often; trying again later";
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
