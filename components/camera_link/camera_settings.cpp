#include "camera_settings.hpp"

#include <algorithm>
#include <cctype>

#include "artemis_json.hpp"

namespace camera::settings
{

namespace
{

using artemis::json_find_bool;
using artemis::json_find_int;
using artemis::json_find_string;
using artemis::json_quote;

// The allowlist (docs/camera-settings.md). Ranges are what the app offers or
// what the field can mean; the camera's capability narrows the resolutions
// and the languages.
constexpr Field kFields[] = {
    {"workMode", Kind::number, 0, 2, nullptr, Choices::none},
    {"shootMode", Kind::number, 0, 2, nullptr, Choices::none},
    {"picResolution", Kind::number, 0, 99, nullptr, Choices::image},
    {"videoResolution", Kind::number, 0, 99, nullptr, Choices::video},
    {"continuousShootCnt", Kind::number, 1, 5, nullptr, Choices::none},
    {"recordDuration", Kind::number, 1, 600, nullptr, Choices::none},
    {"recordAudio", Kind::number, 0, 1, nullptr, Choices::none},
    {"timelapseInterval", Kind::number, 1, 86400, nullptr, Choices::none},
    {"pirSens", Kind::number, 0, 2, nullptr, Choices::none},
    {"pirCoolingTime", Kind::number, 0, 3600, nullptr, Choices::none},
    {"sidePirOnOff", Kind::number, 0, 1, "supportSidePir", Choices::none},
    {"workTimeOnOff", Kind::number, 0, 1, nullptr, Choices::none},
    {"workTimeStartHour", Kind::number, 0, 23, nullptr, Choices::none},
    {"workTimeStartMin", Kind::number, 0, 59, nullptr, Choices::none},
    {"workTimeEndHour", Kind::number, 0, 23, nullptr, Choices::none},
    {"workTimeEndMin", Kind::number, 0, 59, nullptr, Choices::none},
    {"enableAiFilter", Kind::number, 0, 1, "enableAiFilter", Choices::none},
    {"detectionAreaVertex0X", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex0Y", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex1X", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex1Y", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex2X", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex2Y", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex3X", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"detectionAreaVertex3Y", Kind::number, 0, 1000, "enableAiFilter",
     Choices::none},
    {"infraRedLum", Kind::number, 1, 3, "supportInfraRed", Choices::none},
    {"osdInfoEnable", Kind::flag, 0, 1, "supportOsdInfo", Choices::none},
    {"WiFiLedEnable", Kind::flag, 0, 1, "supportWiFiLED", Choices::none},
    {"dateFormat", Kind::number, 0, 2, nullptr, Choices::none},
    {"timeFormat", Kind::number, 0, 2, nullptr, Choices::none},
    {"languageCode", Kind::language, 0, 0, nullptr, Choices::language},
    {"micVol", Kind::number, 0, 100, "supportMicVolume", Choices::none},
    {"spkVol", Kind::number, 0, 100, "supportSpkVolume", Choices::none},
};
constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
constexpr std::size_t kMaxLanguageLength = 8;

std::size_t field_index(const std::string &name)
{
    for (std::size_t i = 0; i < kFieldCount; i++) {
        if (name == kFields[i].name) {
            return i;
        }
    }
    return kFieldCount;
}

bool valid_language(const std::string &code)
{
    if (code.empty() || code.size() > kMaxLanguageLength) {
        return false;
    }
    return std::all_of(code.begin(), code.end(), [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '-';
    });
}

// A field name from outside for an error message: printable, short.
std::string printable(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        if (out.size() >= 40) {
            break;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out += c;
        }
    }
    return out.empty() ? "?" : out;
}

std::size_t skip_space(const std::string &json, std::size_t pos)
{
    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
        pos++;
    }
    return pos;
}

// Index just past the string starting at `pos` (which holds '"'); npos if
// it does not end.
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

// Index of the bracket closing the one at `open`; npos if unbalanced.
std::size_t matching_close(const std::string &json, std::size_t open)
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
        if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
        i++;
    }
    return std::string::npos;
}

// Position of the value of `"key":` at or after `from`; npos if absent.
std::size_t value_at(const std::string &json, const std::string &key,
                     std::size_t from = 0)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t at = json.find(needle, from);
    if (at == std::string::npos) {
        return std::string::npos;
    }
    std::size_t pos = skip_space(json, at + needle.size());
    if (pos >= json.size() || json[pos] != ':') {
        return std::string::npos;
    }
    return skip_space(json, pos + 1);
}

// The elements of the array under `key`, each as its own substring.
std::vector<std::string> array_elements(const std::string &json,
                                        const std::string &key)
{
    std::vector<std::string> out;
    const std::size_t open = value_at(json, key);
    if (open == std::string::npos || json[open] != '[') {
        return out;
    }
    const std::size_t close = matching_close(json, open);
    if (close == std::string::npos) {
        return out;
    }
    std::size_t pos = skip_space(json, open + 1);
    while (pos < close) {
        std::size_t end = pos;
        if (json[pos] == '{' || json[pos] == '[') {
            end = matching_close(json, pos);
            if (end == std::string::npos || end > close) {
                break;
            }
            end++;
        } else if (json[pos] == '"') {
            end = skip_string(json, pos);
            if (end == std::string::npos || end > close) {
                break;
            }
        } else {
            while (end < close && json[end] != ',') {
                end++;
            }
        }
        out.push_back(json.substr(pos, end - pos));
        pos = skip_space(json, end);
        if (pos < close && json[pos] == ',') {
            pos = skip_space(json, pos + 1);
        } else {
            break;
        }
    }
    return out;
}

bool element_int(const std::string &element, std::int64_t &out)
{
    if (!element.empty() && element[0] == '{') {
        return json_find_int(element, "resolution", out);
    }
    return json_find_int("{\"v\":" + element + "}", "v", out);
}

bool element_string(const std::string &element, std::string &out)
{
    if (!element.empty() && element[0] == '{') {
        return json_find_string(element, "langCode", out);
    }
    return json_find_string("{\"v\":" + element + "}", "v", out);
}

std::vector<std::int64_t> int_list(const std::string &json,
                                   const std::string &key)
{
    std::vector<std::int64_t> out;
    for (const std::string &element : array_elements(json, key)) {
        std::int64_t n = 0;
        if (element_int(element, n) && n >= 0 && n <= 99 &&
            std::find(out.begin(), out.end(), n) == out.end()) {
            out.push_back(n);
        }
    }
    return out;
}

std::vector<std::string> language_list(const std::string &json)
{
    std::vector<std::string> out;
    for (const std::string &element : array_elements(json, "languages")) {
        std::string code;
        if (element_string(element, code) && valid_language(code) &&
            std::find(out.begin(), out.end(), code) == out.end()) {
            out.push_back(code);
        }
    }
    return out;
}

// The value as JSON: a number, true / false or a string.
std::string value_json(const Field &field, const Value &value)
{
    switch (field.kind) {
    case Kind::number:
        return std::to_string(static_cast<long long>(value.number));
    case Kind::flag:
        return value.number != 0 ? "true" : "false";
    case Kind::language:
        return json_quote(value.text);
    }
    return "0";
}

std::string values_json(const std::vector<Value> &values, bool leading_comma)
{
    std::string out;
    for (const Value &value : values) {
        const Field *field = find_field(value.name);
        if (field == nullptr) {
            continue;
        }
        if (leading_comma || !out.empty()) {
            out += ',';
        }
        out += "\"" + value.name + "\":" + value_json(*field, value);
    }
    return out;
}

template <typename T, typename F>
std::string json_array(const std::vector<T> &items, F item)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); i++) {
        out += (i > 0 ? "," : "") + item(items[i]);
    }
    return out + "]";
}

void sort_by_table(std::vector<Value> &values)
{
    std::stable_sort(values.begin(), values.end(),
                     [](const Value &a, const Value &b) {
                         return field_index(a.name) < field_index(b.name);
                     });
}

// A value of the change from text: "true"/"false", an integer or a string.
struct Token {
    enum class Type { number, boolean, string } type = Type::number;
    std::int64_t number = 0;
    std::string text;
};

// Reads one flat value at `pos`; false for anything nested or unreadable.
bool read_token(const std::string &json, std::size_t &pos, Token &out)
{
    if (pos >= json.size()) {
        return false;
    }
    const char c = json[pos];
    if (c == '"') {
        const std::size_t end = skip_string(json, pos);
        if (end == std::string::npos) {
            return false;
        }
        out.type = Token::Type::string;
        out.text = json.substr(pos + 1, end - pos - 2);
        if (out.text.find('\\') != std::string::npos) {
            return false;
        }
        pos = end;
        return true;
    }
    if (json.compare(pos, 4, "true") == 0) {
        out.type = Token::Type::boolean;
        out.number = 1;
        pos += 4;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        out.type = Token::Type::boolean;
        out.number = 0;
        pos += 5;
        return true;
    }
    bool negative = false;
    if (c == '-') {
        negative = true;
        pos++;
    }
    const std::size_t digits = pos;
    std::int64_t n = 0;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos]))) {
        if (pos - digits >= 12) {
            return false;
        }
        n = n * 10 + (json[pos] - '0');
        pos++;
    }
    if (pos == digits) {
        return false;
    }
    // No fractions or exponents.
    if (pos < json.size() &&
        (json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E')) {
        return false;
    }
    out.type = Token::Type::number;
    out.number = negative ? -n : n;
    return true;
}

}  // namespace

const Field *fields(std::size_t &count)
{
    count = kFieldCount;
    return kFields;
}

const Field *find_field(const std::string &name)
{
    const std::size_t i = field_index(name);
    return i < kFieldCount ? &kFields[i] : nullptr;
}

bool Capability::supports(const Field &field) const
{
    return field.support == nullptr ||
           std::find(unsupported.begin(), unsupported.end(), field.support) ==
               unsupported.end();
}

const Value *Settings::find(const std::string &name) const
{
    for (const Value &value : values) {
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

Settings parse_settings(const std::string &json)
{
    Settings out;
    // The capability object, cut out so that its flags (enableAiFilter) are
    // not taken for the settings of the same name.
    std::string top = json;
    std::string capability;
    const std::size_t key = json.find("\"capability\"");
    if (key != std::string::npos) {
        const std::size_t open = value_at(json, "capability", key);
        const std::size_t close = open != std::string::npos && json[open] == '{'
                                      ? matching_close(json, open)
                                      : std::string::npos;
        if (close != std::string::npos) {
            capability = json.substr(open, close - open + 1);
            top = json.substr(0, key) + json.substr(close + 1);
        }
    }

    for (const Field &field : kFields) {
        Value value;
        value.name = field.name;
        bool found = false;
        switch (field.kind) {
        case Kind::number:
            found = json_find_int(top, field.name, value.number);
            break;
        case Kind::flag: {
            bool b = false;
            if (json_find_bool(top, field.name, b)) {
                value.number = b ? 1 : 0;
                found = true;
            } else if (json_find_int(top, field.name, value.number)) {
                value.number = value.number != 0 ? 1 : 0;
                found = true;
            }
            break;
        }
        case Kind::language:
            found = json_find_string(top, field.name, value.text) &&
                    valid_language(value.text);
            break;
        }
        if (found) {
            out.values.push_back(value);
        }
    }

    if (!capability.empty()) {
        Capability &cap = out.capability;
        cap.present = true;
        cap.image = int_list(capability, "imgResolutions");
        cap.video = int_list(capability, "videoResolutions");
        cap.languages = language_list(capability);
        for (const Field &field : kFields) {
            bool b = true;
            if (field.support != nullptr &&
                json_find_bool(capability, field.support, b) && !b &&
                std::find(cap.unsupported.begin(), cap.unsupported.end(),
                          field.support) == cap.unsupported.end()) {
                cap.unsupported.push_back(field.support);
            }
        }
    }
    return out;
}

std::string stored_fields(const Settings &settings)
{
    std::string out = values_json(settings.values, true);
    if (!settings.capability.present) {
        return out;
    }
    const Capability &cap = settings.capability;
    const auto resolution = [](std::int64_t n) {
        return "{\"resolution\":" + std::to_string(static_cast<long long>(n)) +
               "}";
    };
    std::string inner =
        "\"languages\":" +
        json_array(cap.languages,
                   [](const std::string &code) {
                       return "{\"langCode\":" + json_quote(code) + "}";
                   }) +
        ",\"imgResolutions\":" + json_array(cap.image, resolution) +
        ",\"videoResolutions\":" + json_array(cap.video, resolution);
    for (const std::string &flag : cap.unsupported) {
        inner += ",\"" + flag + "\":false";
    }
    return out + ",\"capability\":{" + inner + "}";
}

std::string report_fields(const Settings &settings)
{
    std::vector<Value> supported;
    for (const Value &value : settings.values) {
        const Field *field = find_field(value.name);
        if (field != nullptr && settings.capability.supports(*field)) {
            supported.push_back(value);
        }
    }
    if (supported.empty()) {
        return "";
    }
    std::string out = "\"settings\":{" + values_json(supported, false) + "}";
    if (settings.capability.present) {
        const Capability &cap = settings.capability;
        const auto number = [](std::int64_t n) {
            return std::to_string(static_cast<long long>(n));
        };
        out += ",\"capability\":{\"imgResolutions\":" +
               json_array(cap.image, number) +
               ",\"videoResolutions\":" + json_array(cap.video, number) +
               ",\"languages\":" +
               json_array(
                   cap.languages,
                   [](const std::string &code) { return json_quote(code); }) +
               "}";
    }
    return out;
}

bool parse_change(const std::string &object, std::vector<Value> &out,
                  std::string &error)
{
    out.clear();
    error.clear();
    std::size_t pos = skip_space(object, 0);
    if (pos >= object.size() || object[pos] != '{') {
        error = "the settings are not an object";
        return false;
    }
    pos = skip_space(object, pos + 1);
    if (pos < object.size() && object[pos] == '}') {
        error = "no setting to change";
        return false;
    }
    while (pos < object.size()) {
        if (object[pos] != '"') {
            error = "the settings cannot be read";
            return false;
        }
        const std::size_t key_end = skip_string(object, pos);
        if (key_end == std::string::npos) {
            error = "the settings cannot be read";
            return false;
        }
        const std::string key = object.substr(pos + 1, key_end - pos - 2);
        pos = skip_space(object, key_end);
        if (pos >= object.size() || object[pos] != ':') {
            error = "the settings cannot be read";
            return false;
        }
        pos = skip_space(object, pos + 1);
        const Field *field = find_field(key);
        if (field == nullptr) {
            error = "the hub does not change " + printable(key);
            return false;
        }
        Token token;
        if (!read_token(object, pos, token)) {
            error =
                std::string("the value of ") + field->name + " cannot be read";
            return false;
        }
        Value value;
        value.name = field->name;
        bool ok = false;
        switch (field->kind) {
        case Kind::number:
            ok = token.type == Token::Type::number;
            value.number = token.number;
            break;
        case Kind::flag:
            ok = token.type == Token::Type::boolean ||
                 (token.type == Token::Type::number &&
                  (token.number == 0 || token.number == 1));
            value.number = token.number;
            break;
        case Kind::language:
            ok = token.type == Token::Type::string;
            value.text = token.text;
            break;
        }
        if (!ok) {
            error = std::string("the value of ") + field->name +
                    " has the wrong type";
            return false;
        }
        for (const Value &seen : out) {
            if (seen.name == value.name) {
                error = std::string(field->name) + " is given twice";
                return false;
            }
        }
        out.push_back(value);
        pos = skip_space(object, pos);
        if (pos < object.size() && object[pos] == ',') {
            pos = skip_space(object, pos + 1);
            continue;
        }
        if (pos < object.size() && object[pos] == '}') {
            sort_by_table(out);
            return true;
        }
        break;
    }
    error = "the settings cannot be read";
    out.clear();
    return false;
}

std::string check_change(const std::vector<Value> &change,
                         const Settings &current)
{
    if (change.empty()) {
        return "no setting to change";
    }
    const Capability &cap = current.capability;
    for (const Value &value : change) {
        const Field *field = find_field(value.name);
        if (field == nullptr) {
            return "the hub does not change " + printable(value.name);
        }
        if (cap.present && !cap.supports(*field)) {
            return std::string("the camera does not support ") + field->name;
        }
        if (field->kind == Kind::language) {
            if (!valid_language(value.text)) {
                return std::string("the value of ") + field->name +
                       " is not a language code";
            }
            if (cap.present && !cap.languages.empty() &&
                std::find(cap.languages.begin(), cap.languages.end(),
                          value.text) == cap.languages.end()) {
                return "the camera does not offer the language " + value.text;
            }
            continue;
        }
        if (value.number < field->min || value.number > field->max) {
            return std::string(field->name) + " must be " +
                   std::to_string(static_cast<long long>(field->min)) + " to " +
                   std::to_string(static_cast<long long>(field->max));
        }
        const std::vector<std::int64_t> *choices =
            field->choices == Choices::image   ? &cap.image
            : field->choices == Choices::video ? &cap.video
                                               : nullptr;
        if (cap.present && choices != nullptr && !choices->empty() &&
            std::find(choices->begin(), choices->end(), value.number) ==
                choices->end()) {
            return std::string("the camera does not offer ") + field->name +
                   " " + std::to_string(static_cast<long long>(value.number));
        }
    }
    return "";
}

std::string request_fields(const std::vector<Value> &change)
{
    return values_json(change, true);
}

std::string encode_change(const std::vector<Value> &change)
{
    std::string out;
    for (const Value &value : change) {
        const Field *field = find_field(value.name);
        if (field == nullptr) {
            continue;
        }
        if (!out.empty()) {
            out += ';';
        }
        out += value.name + "=";
        out += field->kind == Kind::language
                   ? value.text
                   : std::to_string(static_cast<long long>(value.number));
    }
    return out;
}

bool decode_change(const std::string &text, std::vector<Value> &out)
{
    out.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(';', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string part = text.substr(start, end - start);
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) {
            out.clear();
            return false;
        }
        const Field *field = find_field(part.substr(0, eq));
        const std::string raw = part.substr(eq + 1);
        if (field == nullptr || raw.empty()) {
            out.clear();
            return false;
        }
        Value value;
        value.name = field->name;
        if (field->kind == Kind::language) {
            if (!valid_language(raw)) {
                out.clear();
                return false;
            }
            value.text = raw;
        } else {
            std::size_t pos = 0;
            Token token;
            if (!read_token(raw, pos, token) || pos != raw.size() ||
                token.type != Token::Type::number) {
                out.clear();
                return false;
            }
            value.number = token.number;
        }
        out.push_back(value);
        start = end + 1;
    }
    return !out.empty();
}

void apply_change(Settings &settings, const std::vector<Value> &change)
{
    for (const Value &value : change) {
        if (find_field(value.name) == nullptr) {
            continue;
        }
        bool replaced = false;
        for (Value &held : settings.values) {
            if (held.name == value.name) {
                held = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            settings.values.push_back(value);
        }
    }
    sort_by_table(settings.values);
}

}  // namespace camera::settings
