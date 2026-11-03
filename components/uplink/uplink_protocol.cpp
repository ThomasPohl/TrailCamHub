#include "uplink_protocol.hpp"

#include <cstdio>

#include "artemis_json.hpp"

namespace uplink
{

namespace
{

using camera::artemis::json_find_int;
using camera::artemis::json_find_string;

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

// Index of the bracket closing the one at `open`, honouring nesting and
// strings; npos if unbalanced.
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

// The objects of the array stored under `key`, each as its own substring.
std::vector<std::string> array_objects(const std::string &json,
                                       const std::string &key)
{
    std::vector<std::string> objects;
    const std::size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) {
        return objects;
    }
    const std::size_t open =
        json.find_first_not_of(" \t\r\n:", key_pos + key.size() + 2);
    if (open == std::string::npos || json[open] != '[') {
        return objects;
    }
    const std::size_t close = matching_close(json, open);
    if (close == std::string::npos) {
        return objects;
    }
    std::size_t pos = open + 1;
    while (pos < close) {
        const std::size_t begin = json.find('{', pos);
        if (begin == std::string::npos || begin > close) {
            break;
        }
        const std::size_t end = matching_close(json, begin);
        if (end == std::string::npos || end > close) {
            break;
        }
        objects.push_back(json.substr(begin, end - begin + 1));
        pos = end + 1;
    }
    return objects;
}

void append_media(std::string &out, const MediaRef &m)
{
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"mediaId\":%lld,\"dir\":%lld,\"num\":%lld,"
                  "\"fileType\":%lld,\"mediaTime\":%lld}",
                  (long long)m.media_id, (long long)m.dir, (long long)m.num,
                  (long long)m.file_type, (long long)m.media_time);
    out += buffer;
}

// The object stored under `key`, as its own substring; empty if missing.
std::string object_of(const std::string &json, const std::string &key)
{
    const std::size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) {
        return "";
    }
    const std::size_t open =
        json.find_first_not_of(" \t\r\n:", key_pos + key.size() + 2);
    if (open == std::string::npos || json[open] != '{') {
        return "";
    }
    const std::size_t close = matching_close(json, open);
    if (close == std::string::npos) {
        return "";
    }
    return json.substr(open, close - open + 1);
}

// Reads a boolean field; leaves `out` unchanged if it is missing.
void find_bool(const std::string &json, const std::string &key, bool &out)
{
    const std::size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) {
        return;
    }
    const std::size_t value =
        json.find_first_not_of(" \t\r\n:", key_pos + key.size() + 2);
    if (value == std::string::npos) {
        return;
    }
    if (json.compare(value, 4, "true") == 0) {
        out = true;
    } else if (json.compare(value, 5, "false") == 0) {
        out = false;
    }
}

}  // namespace

std::string json_escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string build_checkin_json(const CheckinRequest &request)
{
    std::string out =
        "{\"hubFirmware\":\"" + json_escape(request.hub_firmware) + "\"";
    if (request.hub_bat_percent >= 0) {
        out += ",\"hubBatPercent\":" + std::to_string(request.hub_bat_percent);
    }
    const ScheduleReport &schedule = request.schedule;
    if (schedule.present) {
        out += ",\"schedule\":{\"cameraPlan\":\"" +
               json_escape(schedule.camera_plan) +
               "\",\"cameraPlanOrigin\":\"" +
               json_escape(schedule.camera_plan_origin) +
               "\",\"checkinIntervalS\":" +
               std::to_string(schedule.checkin_interval_s) +
               ",\"checkinOrigin\":\"" + json_escape(schedule.checkin_origin) +
               "\"";
        if (schedule.next_camera_read_at > 0) {
            out += ",\"nextCameraReadAt\":" +
                   std::to_string(schedule.next_camera_read_at);
        }
        if (schedule.next_checkin_at > 0) {
            out += ",\"nextCheckinAt\":" +
                   std::to_string(schedule.next_checkin_at);
        }
        out += schedule.sleeps ? ",\"sleeps\":true}" : ",\"sleeps\":false}";
    }
    out += ",\"cameras\":[";
    for (std::size_t i = 0; i < request.cameras.size(); i++) {
        const CameraReport &c = request.cameras[i];
        if (i > 0) {
            out += ",";
        }
        out += "{\"camId\":\"" + json_escape(c.cam_id) + "\"";
        if (c.read_at > 0) {
            out += ",\"readAt\":" + std::to_string(c.read_at);
        }
        if (!c.model.empty()) {
            out += ",\"model\":\"" + json_escape(c.model) + "\"";
        }
        if (!c.firmware.empty()) {
            out += ",\"firmware\":\"" + json_escape(c.firmware) + "\"";
        }
        if (c.bat_percent >= 0) {
            out += ",\"batPercent\":" + std::to_string(c.bat_percent);
        }
        if (c.sd_free_mb >= 0) {
            out += ",\"sdFreeMB\":" + std::to_string(c.sd_free_mb);
        }
        out += ",\"media\":[";
        bool first = true;
        for (const MediaRef &m : c.media) {
            if (m.file_type != 0 && m.file_type != 2) {
                continue;
            }
            if (!first) {
                out += ",";
            }
            append_media(out, m);
            first = false;
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

bool ack_is_empty(const AckRequest &request)
{
    for (const CameraAck &c : request.cameras) {
        if (!c.uploaded_thumbs.empty() || !c.commands.empty()) {
            return false;
        }
    }
    return true;
}

std::string build_ack_json(const AckRequest &request)
{
    std::string out = "{\"cameras\":[";
    bool first_camera = true;
    for (const CameraAck &camera : request.cameras) {
        if (camera.uploaded_thumbs.empty() && camera.commands.empty()) {
            continue;
        }
        if (!first_camera) {
            out += ",";
        }
        first_camera = false;
        out += "{\"camId\":\"" + json_escape(camera.cam_id) +
               "\",\"uploadedThumbs\":[";
        for (std::size_t i = 0; i < camera.uploaded_thumbs.size(); i++) {
            if (i > 0) {
                out += ",";
            }
            out += "{\"mediaId\":" + std::to_string(camera.uploaded_thumbs[i]) +
                   "}";
        }
        out += "],\"commands\":[";
        for (std::size_t i = 0; i < camera.commands.size(); i++) {
            const CommandResult &c = camera.commands[i];
            if (i > 0) {
                out += ",";
            }
            out += "{\"id\":\"" + json_escape(c.id) + "\",\"state\":\"" +
                   (c.done ? "done" : "failed") + "\"";
            if (!c.done && !c.error.empty()) {
                out += ",\"error\":\"" + json_escape(c.error) + "\"";
            }
            out += "}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

bool parse_checkin_response(const std::string &json, CheckinResponse &out)
{
    out = CheckinResponse{};
    if (!json_find_int(json, "serverTime", out.server_time)) {
        return false;
    }
    const std::string config = object_of(json, "config");
    if (!config.empty()) {
        out.has_camera_plan =
            json_find_string(config, "cameraPlan", out.camera_plan);
        if (!json_find_int(config, "checkinIntervalS",
                           out.checkin_interval_s) ||
            out.checkin_interval_s < 0) {
            out.checkin_interval_s = -1;
        }
    }

    for (const std::string &camera_json : array_objects(json, "cameras")) {
        CameraCheckin camera;
        if (!json_find_string(camera_json, "camId", camera.cam_id) ||
            camera.cam_id.empty()) {
            continue;
        }
        find_bool(camera_json, "accepted", camera.accepted);
        find_bool(camera_json, "resync", camera.resync);
        for (const std::string &obj :
             array_objects(camera_json, "thumbUploads")) {
            ThumbUpload upload;
            if (json_find_int(obj, "mediaId", upload.media_id) &&
                upload.media_id > 0 &&
                json_find_string(obj, "url", upload.url) &&
                upload.url.rfind("https://", 0) == 0) {
                camera.thumb_uploads.push_back(upload);
            }
        }
        for (const std::string &obj : array_objects(camera_json, "commands")) {
            Command command;
            if (json_find_string(obj, "id", command.id) &&
                json_find_string(obj, "type", command.type)) {
                json_find_int(obj, "mediaId", command.media_id);
                camera.commands.push_back(command);
            }
        }
        out.cameras.push_back(camera);
    }
    return true;
}

}  // namespace uplink
