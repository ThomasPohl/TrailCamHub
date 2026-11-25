#pragma once

// Wire format of the backend's hub API v1 (see backend/README.md): building
// the batch check-in and acknowledgement bodies for all cameras of a hub and
// reading the check-in response. Pure logic, host-testable; HTTP lives in
// uplink.cpp.

#include <cstdint>
#include <optional>
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
    // The camera's state at the read; unset/empty = unknown, left out.
    std::optional<std::int64_t> temperature_c;
    std::optional<bool> charging;
    std::optional<bool> solar_charging;
    std::optional<std::int64_t> bat_type;
    std::string ble_version;
    std::string camera_name;
    // BLE address the hub connected to ("AA:BB:CC:DD:EE:FF", #296); empty =
    // unknown, left out.
    std::string ble_address;
    // The camera's settings and capability as camera::settings::report_fields()
    // writes them ("settings":{...},"capability":{...}); inserted as they are,
    // left out when empty (#368).
    std::string settings;
    // Media the backend does not know yet (delta).
    std::vector<MediaRef> media;
    // mediaIds no longer on the camera's card that the backend was not told
    // about yet; left out when empty.
    std::vector<std::int64_t> removed;
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
    // Why the schedule is stretched ("hubBatteryLow", ...); empty: it is not.
    std::string stretch;
};

// Memory headroom of the hub (cycle_trace), so that "RAM is tight" on a board
// becomes numbers from the field.
struct MemoryReport {
    bool present = false;
    // The cycle the figures belong to (the running one at a check-in).
    std::int64_t cycle = 0;
    // Lowest free heap of that cycle in bytes; PSRAM -1: the board has none.
    std::int64_t heap_min_internal = 0;
    std::int64_t heap_min_psram = -1;
    // Lowest free stack of the tasks in bytes; -1 = unknown, left out.
    std::int64_t stack_session = -1;
    std::int64_t stack_uplink = -1;
    std::int64_t stack_controller = -1;
};

// The hub's last firmware update (#149), sent once.
struct FirmwareUpdateReport {
    bool present = false;
    std::string from;
    std::string to;
    std::string result;   // installed, rolledBack, failed, skipped
    std::string reason;   // left out when empty
    std::int64_t at = 0;  // Unix seconds; left out when 0 (clock not set)
};

// A camera heard advertising (#296).
struct DiscoveredCamera {
    std::string ble_address;
    std::string name;  // advertised name; empty = none heard
    std::int64_t rssi = 0;
    std::int64_t seen_at = 0;  // hub clock; 0 = unknown, left out
};

// Data volume since the last acknowledged check-in (#415), per transport; an
// estimate (HTTP bytes plus a fixed share for TLS and TCP/IP). Entries
// without bytes are left out.
struct DataUsageEntry {
    std::string transport;  // "wifi", "modem", "ethernet"
    std::uint64_t tx_bytes = 0;
    std::uint64_t rx_bytes = 0;
};

// What this firmware does beyond the basics (#461), for the check-in's
// `features`: the backend sends only what a firmware can do.
//   deleteTargets: knows the `targets` of a deleteMedia command (the hub's own
//                  files, with or without the camera's).
inline constexpr const char *kFeatureDeleteTargets = "deleteTargets";

struct CheckinRequest {
    std::string hub_firmware;
    // Left out when empty (see kFeatureDeleteTargets).
    std::vector<std::string> features;
    // Board and build variant ("esp32s3", "release"): which firmware image
    // fits the hub. Left out when empty.
    std::string hub_board;
    std::string hub_variant;
    std::int64_t hub_bat_percent = -1;  // -1 = unknown
    // The cell is being charged, -1 = unknown (#421). The gauge reads 100 %
    // while charging, so the percentage means little then.
    int hub_charging = -1;
    std::int64_t hub_bat_millivolts = -1;  // cell voltage, -1 = unknown
    // How this check-in reaches the backend (#149): "wifi", "modem" or
    // "ethernet"; left out when empty.
    std::string hub_uplink;
    // The resolved mode, "mains" or "battery" (#416): lets the backend flag a
    // battery that is left on mains. Left out when empty.
    std::string hub_power_mode;
    // Modem signal measured when connecting, dBm; left out when 0 (unknown
    // or no modem).
    int modem_signal_dbm = 0;
    // Name of the mobile network (modem_uplink::operator_name()); left out
    // when empty.
    std::string modem_operator;
    // The most thumbnail uploads the backend should answer with; -1 leaves
    // it to the backend (30). Each one is a presigned URL of about 1.8 KB in
    // the response (measured on the WROOM-32D, #244). The backend hands out
    // as many video requests as this (at least one), as they carry a URL too.
    std::int64_t max_thumb_uploads = -1;
    ScheduleReport schedule;
    MemoryReport memory;
    std::vector<DataUsageEntry> data_usage;  // left out when empty
    FirmwareUpdateReport firmware_update;
    std::vector<CameraReport> cameras;
    // Cameras heard in BLE scans, read or not (#296). Sent as "discovered"
    // when `report_discovered` is set, even if empty, so the backend knows
    // this firmware reports BLE addresses.
    bool report_discovered = false;
    std::vector<DiscoveredCamera> discovered;
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
    // setSettings (#368): the "settings" object as it came; empty if none.
    std::string settings;
    // deleteMedia (#461): where it applies ("camera", "hub"); `has_targets`
    // is false if the command had none (an older backend: the camera).
    bool has_targets = false;
    std::vector<std::string> targets;
};

struct CameraCheckin {
    std::string cam_id;
    bool accepted = true;
    // The backend is new to this camera: report the full media list next time.
    bool resync = false;
    std::vector<ThumbUpload> thumb_uploads;
    std::vector<Command> commands;
};

struct CheckinResponse {
    std::int64_t server_time = 0;
    // Schedule set for this hub in the backend ("config"); a missing value
    // means none is set there.
    bool has_camera_plan = false;
    std::string camera_plan;
    std::int64_t checkin_interval_s = -1;  // -1 = not set
    // The hub's `power` setting from the gallery (#197): "auto", "mains" or
    // "battery"; empty = not set (the hub keeps its own). Any other value is
    // dropped by the parser.
    std::string power_setting;
    // BLE addresses of the cameras approved in the gallery (#297, #299);
    // absent (old backend) = leave the stored list as it is.
    bool has_approved_cameras = false;
    std::vector<std::string> approved_cameras;
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
