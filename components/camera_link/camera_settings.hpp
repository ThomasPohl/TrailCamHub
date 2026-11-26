#pragma once

// The camera's settings as the device info (512) reports them, and changes
// to them with SET_DEV_PARAM (513), see docs/camera-settings.md (#197, #368).
// Pure logic, host-tested.
//
// Only the fields of the table in camera_settings.cpp are read, stored, sent
// on or written (an allowlist). Left out on purpose:
//   - the screen lock (secureCodeOnOff, secureCode0..3): the digits are a
//     secret (json_is_secret_key);
//   - the clock (timeUTC / utcTime): the hub never writes the camera time
//     back (docs/clock.md);
//   - fields whose meaning is unknown (recordMode, lum, pwrFreq, GPS,
//     customName) and the camera name in the picture.
// All other fields use the same name for reading and writing.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camera::settings
{

enum class Kind : std::uint8_t {
    number,    // an integer in [min, max]
    flag,      // a JSON boolean (kept as 0 / 1)
    language,  // a language code string from capability.languages
};

// Where the allowed values of a field come from, besides min/max.
enum class Choices : std::uint8_t {
    none,
    image,     // capability.imgResolutions
    video,     // capability.videoResolutions
    language,  // capability.languages
};

struct Field {
    const char *name;
    Kind kind;
    std::int64_t min;
    std::int64_t max;
    // The capability flag that must not be false for the field to be used;
    // nullptr: always there.
    const char *support;
    Choices choices;
};

// The allowlist, in a fixed order (the order of the check-in and the 513).
const Field *fields(std::size_t &count);
// nullptr for a name that is not in the allowlist.
const Field *find_field(const std::string &name);

// One value: `number` for numbers and flags (0 / 1), `text` for a language.
struct Value {
    std::string name;
    std::int64_t number = 0;
    std::string text;
};

// What the camera says it can do (`capability` in 512).
struct Capability {
    bool present = false;
    std::vector<std::int64_t> image;  // imgResolutions
    std::vector<std::int64_t> video;  // videoResolutions
    std::vector<std::string> languages;
    // The support flags the camera reports as false, e.g. "supportSidePir".
    std::vector<std::string> unsupported;

    // False if the field's support flag is reported false.
    bool supports(const Field &field) const;
};

struct Settings {
    std::vector<Value> values;
    Capability capability;

    bool empty() const
    {
        return values.empty() && !capability.present;
    }
    const Value *find(const std::string &name) const;
};

// Reads the allowlisted settings and the capability from a device info (512)
// answer or from the stored copy (camera.json, same form). Fields outside the
// allowlist are ignored, also inside `capability`.
Settings parse_settings(const std::string &json);

// The settings and the capability as more fields of a flat object, in the 512
// form, each with a leading comma (",\"workMode\":0,...,\"capability\":{...}");
// empty if nothing is known. parse_settings() reads it back.
std::string stored_fields(const Settings &settings);

// The check-in form ("settings":{...},"capability":{...}, without a leading
// comma): only the fields the camera supports, the capability lists as plain
// arrays ("imgResolutions":[1,2]). Empty if no setting is known.
std::string report_fields(const Settings &settings);

// A change as the backend sends it: a flat JSON object of known fields, a
// number (or for flags true / false / 0 / 1, for the language a string) each.
// False with `error` for anything else: an unknown or forbidden field, a
// value of the wrong kind, no field at all. Only the type is checked here;
// check_change() checks the values.
bool parse_change(const std::string &object, std::vector<Value> &out,
                  std::string &error);

// Empty if the camera can take the change, else why not: a value out of
// range or not in the camera's capability, a field it does not support.
// `current` may be empty (no device info known): then only the ranges count.
std::string check_change(const std::vector<Value> &change,
                         const Settings &current);

// The fields that follow "cmdId" in the 513 request, e.g.
// ",\"workMode\":1,\"osdInfoEnable\":true,\"languageCode\":\"de\"".
std::string request_fields(const std::vector<Value> &change);

// A change as one line of the action queue: "workMode=1;languageCode=de"
// (no commas). decode_change() is the inverse; false for text it cannot read
// or for an unknown field.
std::string encode_change(const std::vector<Value> &change);
bool decode_change(const std::string &text, std::vector<Value> &out);

// The settings after the camera accepted the change.
void apply_change(Settings &settings, const std::vector<Value> &change);

// The console form (#197): the supported settings as "workMode=0 pirSens=2
// languageCode=de" (flags as 0 / 1), the same words `camset` takes; empty if
// none is known.
std::string console_line(const Settings &settings);

// The capability for the console: "images=1,2 videos=3,4 languages=de,en
// unsupported=supportSidePir" (empty parts left out); empty if the camera
// reported none.
std::string capability_line(const Capability &capability);

// A change as the flat JSON object request_settings() and the backend take,
// e.g. {"pirSens":2,"osdInfoEnable":true,"languageCode":"de"}.
std::string change_object(const std::vector<Value> &change);

}  // namespace camera::settings
