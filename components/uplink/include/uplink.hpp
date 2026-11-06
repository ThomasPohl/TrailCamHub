#pragma once

// One sync with the TrailCamHub backend over the home Wi-Fi (already
// joined): a single check-in for every camera stored on the SD card (with the
// media the backend does not know yet), upload of the thumbnails the backend
// is missing, and one acknowledgement for uploads and commands.

#include <cstdint>
#include <string>

#include "esp_err.h"

#include "hub_config_rules.hpp"

namespace uplink
{

struct Report {
    int cameras_reported = 0;   // cameras in the check-in
    int cameras_refused = 0;    // ... that belong to another hub
    int media_reported = 0;     // media entries in the check-in
    int thumbs_requested = 0;   // thumbnails the backend asked for
    int thumbs_uploaded = 0;    // ... uploaded successfully
    int thumbs_not_on_sd = 0;   // ... not stored on the SD card yet
    int commands_received = 0;  // commands handed out by the backend
    int videos_uploaded = 0;    // requested videos uploaded as MP4
    std::int64_t server_time = 0;
    std::string error;  // set when the sync failed
};

esp_err_t run(const hub_config::Settings &settings, Report &report);

}  // namespace uplink
