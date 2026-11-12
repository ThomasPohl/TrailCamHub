#include "camera_action.hpp"

#include <cctype>

#include "artemis_json.hpp"

namespace camera::action
{

const char *type_name(Type type)
{
    switch (type) {
    case Type::photo:
        return "photo";
    case Type::video_start:
        return "video-start";
    case Type::video_stop:
        return "video-stop";
    case Type::delete_media:
        return "delete";
    case Type::format_sd:
        return "format";
    }
    return "unknown";
}

std::optional<Type> parse_type(const std::string &name)
{
    for (const Type type : {Type::photo, Type::video_start, Type::video_stop,
                            Type::delete_media, Type::format_sd}) {
        if (name == type_name(type)) {
            return type;
        }
    }
    return std::nullopt;
}

std::uint32_t cmd_id(Type type)
{
    switch (type) {
    case Type::photo:
        return 641;
    case Type::video_start:
        return 643;
    case Type::video_stop:
        return 644;
    case Type::delete_media:
        return 773;
    case Type::format_sd:
        return 518;
    }
    return 0;
}

std::optional<Type> type_of_cmd(std::int64_t id)
{
    for (const Type type : {Type::photo, Type::video_start, Type::video_stop,
                            Type::delete_media, Type::format_sd}) {
        if (static_cast<std::int64_t>(cmd_id(type)) == id) {
            return type;
        }
    }
    return std::nullopt;
}

const char *source_name(Source source)
{
    switch (source) {
    case Source::console:
        return "console";
    case Source::web_ui:
        return "web";
    case Source::telegram:
        return "telegram";
    case Source::backend:
        return "backend";
    }
    return "unknown";
}

bool allowed(Type type, Source source)
{
    switch (type) {
    case Type::photo:
    case Type::video_start:
    case Type::video_stop:
        return true;
    case Type::delete_media:
        return source == Source::console || source == Source::web_ui;
    case Type::format_sd:
        return source == Source::console || source == Source::web_ui;
    }
    return false;
}

bool needs_confirmation(Type type)
{
    return type == Type::delete_media || type == Type::format_sd;
}

bool needs_media_list(Type type)
{
    return type == Type::delete_media;
}

std::string validate(const Action &action)
{
    if (action.type == Type::delete_media && action.media_id <= 0) {
        return "a deletion needs the mediaId of the recording";
    }
    if (action.type != Type::delete_media && action.media_id != 0) {
        return "only a deletion takes a mediaId";
    }
    return "";
}

std::string check_camera(Type type, const artemis::DeviceInfo &info)
{
    // A card that is there but has no free space: the camera cannot record.
    const bool records = type == Type::photo || type == Type::video_start;
    if (records && info.sd_total_mb > 0 && info.sd_free_mb == 0) {
        return "the camera's SD card is full";
    }
    return "";
}

std::optional<Target> find_target(const artemis::MediaCatalog &catalog,
                                  std::int64_t media_id)
{
    const artemis::CatalogEntry *entry = catalog.find(media_id);
    if (entry == nullptr || !entry->listed) {
        return std::nullopt;
    }
    return Target{entry->file_type, entry->dir_num, entry->media_num};
}

std::string request_fields(const Action &action,
                           const std::optional<Target> &target)
{
    if (action.type != Type::delete_media || !target) {
        return "";
    }
    return ",\"fileType\":" + std::to_string(target->file_type) +
           ",\"dirNum\":" + std::to_string(target->dir_num) +
           ",\"mediaNum\":" + std::to_string(target->media_num);
}

namespace
{

// The field that carries the result of the command itself; empty for 644.
const char *result_field(Type type)
{
    switch (type) {
    case Type::photo:
        return "snapRet";
    case Type::video_start:
        return "recordRet";
    case Type::video_stop:
        return "";
    case Type::delete_media:
        return "delRet";
    case Type::format_sd:
        return "formatRet";
    }
    return "";
}

// The camera's own message, if it is short, plain text and not "Success".
std::string camera_message(const std::string &json)
{
    std::string text;
    if (!artemis::json_find_string(json, "errorMsg", text) || text.empty() ||
        text == "Success" || text.size() > 60) {
        return "";
    }
    for (const char c : text) {
        if (!std::isprint(static_cast<unsigned char>(c))) {
            return "";
        }
    }
    return text;
}

}  // namespace

Outcome interpret(Type type, const std::string &json)
{
    Outcome outcome;
    const std::string field = result_field(type);
    const std::string key = field.empty() ? "result" : field;
    std::int64_t value = 0;
    if (!artemis::json_find_int(json, key, value)) {
        outcome.code = -1;
        outcome.reason = "the answer has no " + key;
        return outcome;
    }
    outcome.code = value;
    if (value == 0) {
        outcome.done = true;
        return outcome;
    }
    if (type == Type::delete_media && value == -3) {
        outcome.reason = "the camera has no such file (already deleted?)";
    } else {
        outcome.reason = "the camera answered " + key + " " +
                         std::to_string(static_cast<long long>(value));
    }
    const std::string message = camera_message(json);
    if (!message.empty()) {
        outcome.reason += ": " + message;
    }
    return outcome;
}

Outcome failure(const std::string &reason)
{
    Outcome outcome;
    outcome.code = -1;
    outcome.reason = reason;
    return outcome;
}

}  // namespace camera::action
