#pragma once

// Push to a WebDAV share (docs/modes.md), the destination besides the own
// backend (uplink::run() picks one).

#include "esp_err.h"

#include "hub_config_rules.hpp"
#include "uplink.hpp"

namespace uplink
{

// Copies new thumbnails, photos and videos of every camera on the SD card
// into the WebDAV folder (one subfolder per camera).
esp_err_t push_webdav(const hub_config::Settings &settings, bool over_modem,
                      Report &report);

}  // namespace uplink
