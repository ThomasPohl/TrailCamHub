#pragma once

// What the parts of the backend sync share (backend.cpp, backend_http.cpp,
// backend_checkin.cpp, backend_commands.cpp). Not part of the component's
// interface: uplink.hpp is.

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

#include "http_util.hpp"
#include "hub_config.hpp"
#include "sync_cameras.hpp"
#include "uplink.hpp"
#include "uplink_protocol.hpp"

namespace uplink::backend
{

// backend_http.cpp
HttpResult post_json(const std::string &url, const std::string &device_key,
                     std::string body);
HttpResult put_file(const std::string &url, const std::string &path);
HttpResult put_mp4(const std::string &url, const std::string &camera_id,
                   std::int64_t media_id, bool over_modem, std::uint64_t &size,
                   std::string &too_large, std::int64_t &sd_read_us);

// backend_checkin.cpp
// What one check-in told the backend about a camera: new media and media
// that are gone from the card.
struct SentIds {
    std::vector<std::int64_t> media;
    std::vector<std::int64_t> removed;
};

// The mediaIds sent with this check-in, per camera, in the order of the
// cameras of the sync.
using SentMedia = std::vector<SentIds>;

void set_clock(std::int64_t unix_seconds);
void collect_cameras(CheckinRequest &request, std::vector<SyncCamera> &cameras,
                     SentMedia &sent);
ScheduleReport schedule_report();
void apply_checkin_result(SyncCamera &camera, const SentIds &sent,
                          const CameraCheckin &result);
MemoryReport memory_report();

// backend_commands.cpp
bool handle_delete_command(const std::string &cam_id, const Command &command,
                           CommandResult &out, bool &read_needed);
bool handle_video_command(SyncCamera *camera, const std::string &cam_id,
                          const Command &command, bool over_modem,
                          CommandResult &out, Report &report);

// uplink.cpp: a newer firmware from GitHub, last in a sync.
void update_firmware(const hub_config::Settings &settings);

// backend.cpp: the sync with the own backend.
esp_err_t run_backend(const hub_config::Settings &settings, Report &report);

}  // namespace uplink::backend
