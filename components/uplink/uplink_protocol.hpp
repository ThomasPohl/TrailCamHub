#pragma once

// Wire format of the backend's hub API v1 (see backend/README.md): building
// the batch check-in and acknowledgement bodies for all cameras of a hub and
// reading the check-in response. Pure logic, host-testable; HTTP lives in
// uplink.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace uplink
{

// A recording is identified by its mediaId (docs/media-identity.md); dir/num
// are only data.
struct MediaRef {
    std::int64_t media_id = 0;
    std::int64_t dir = 0;
    std::int64_t num = 0;
    std::int64_t file_type = 0;   // 0 = photo, 2 = video
    std::int64_t media_time = 0;  // Unix seconds
};

struct CameraReport {
    std::string cam_id;
    std::int64_t read_at = 0;       // hub clock; 0 = unknown
    std::string model;              // empty = unknown
    std::string firmware;           // empty = unknown
    std::int64_t bat_percent = -1;  // -1 = unknown
    std::int64_t sd_free_mb = -1;   // -1 = unknown
    std::int64_t sd_total_mb = -1;  // -1 = unknown
    // The camera's SD card at the last media list: "ok", "missing" or empty
    // (unknown); with "missing" the camera's error message, if any.
    std::string sd_card;
    std::string sd_error;
    // Media the backend does not know yet (delta).
    std::vector<MediaRef> media;
};

// The hub's wake-up schedule as it applies now.
struct ScheduleReport {
    bool present = false;
    std::string camera_plan;              // e.g. "*=120"; "off"
    std::string camera_plan_origin;       // "built-in", "local", "remote"
    std::int64_t checkin_interval_s = 0;  // 0 = none on schedule
    std::string checkin_origin;
    // Unix seconds; 0 = none planned or the clock is not set.
    std::int64_t next_camera_read_at = 0;
    std::int64_t next_checkin_at = 0;
    // The hub sleeps between cycles (it only answers at check-ins).
    bool sleeps = false;
};

struct CheckinRequest {
    std::string hub_firmware;
    // Board and build variant ("esp32s3", "release"): which firmware image
    // fits the hub. Left out when empty.
    std::string hub_board;
    std::string hub_variant;
    std::int64_t hub_bat_percent = -1;  // -1 = unknown
    ScheduleReport schedule;
    std::vector<CameraReport> cameras;
};

struct ThumbUpload {
    std::int64_t media_id = 0;
    std::string url;
};

struct Command {
    std::string id;
    std::string type;
    std::int64_t media_id = 0;  // requestVideo; 0 = none
    // requestVideo: presigned URL to PUT the MP4 to (https only); empty if
    // none was given.
    std::string upload_url;
};

struct CameraCheckin {
    std::string cam_id;
    bool accepted = true;
    // The backend is new to this camera: report the full media list next time.
    bool resync = false;
    std::vector<ThumbUpload> thumb_uploads;
    std::vector<Command> commands;
};

// A newer firmware the backend offers (a GitHub release).
struct FirmwareOffer {
    bool present = false;
    std::string version;  // e.g. "v1.2.3"
    std::string url;      // https only
    std::string sha256;   // lowercase hex, 64 characters
    std::int64_t size = 0;
};

struct CheckinResponse {
    std::int64_t server_time = 0;
    FirmwareOffer firmware;
    // Schedule set for this hub in the backend ("config"); a missing value
    // means none is set there.
    bool has_camera_plan = false;
    std::string camera_plan;
    std::int64_t checkin_interval_s = -1;  // -1 = not set
    std::vector<CameraCheckin> cameras;
};

struct CommandResult {
    std::string id;
    bool done = false;
    std::string error;  // for failed commands
};

struct CameraAck {
    std::string cam_id;
    std::vector<std::int64_t> uploaded_thumbs;  // mediaIds
    std::vector<CommandResult> commands;
};

struct AckRequest {
    std::vector<CameraAck> cameras;
};

// Escapes a string for use inside JSON double quotes.
std::string json_escape(const std::string &text);

// Media entries other than photos (0) and videos (2) are left out, because
// the backend rejects them.
std::string build_checkin_json(const CheckinRequest &request);
// Cameras without uploads or command results are left out.
std::string build_ack_json(const AckRequest &request);
bool ack_is_empty(const AckRequest &request);

// Returns false if the response is not a check-in response (no serverTime).
// Malformed list entries are skipped.
bool parse_checkin_response(const std::string &json, CheckinResponse &out);

}  // namespace uplink
