#pragma once

// Push to a WebDAV share or to Telegram directly (docs/modes.md), the
// destinations besides the own backend (uplink::run() picks one).

#include "esp_err.h"

#include "hub_config_rules.hpp"
#include "uplink.hpp"

namespace uplink
{

// Copies new thumbnails, photos and videos of every camera on the SD card
// into the WebDAV folder (one subfolder per camera).
esp_err_t push_webdav(const hub_config::Settings &settings, bool over_modem,
                      Report &report);

// Handles the bot's commands and buttons since the last check-in, sends new
// thumbnails and requested videos to the paired chat.
esp_err_t push_telegram(const hub_config::Settings &settings, bool over_modem,
                        Report &report);

}  // namespace uplink
