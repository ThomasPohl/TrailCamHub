#pragma once

// Decides what a Wi-Fi "disconnected" event means for the network the station
// is currently trying to join. Kept free of ESP-IDF types so it can be tested
// on the host.
//
// esp_wifi_disconnect() is asynchronous: when the hub leaves the camera
// network and immediately joins the home network, the camera's disconnect
// event arrives while the home join is already under way. Such a stale event
// belongs to the previous network and must not count as a failed attempt.

#include <cstddef>
#include <cstdint>

namespace wifipolicy
{

enum class DisconnectAction {
    ignore,       // not for the wanted network, or no connection wanted
    report_lost,  // an established connection to the wanted network dropped
    retry,        // a join attempt failed; try again
    give_up,      // a join attempt failed and the retries are used up
};

// Whether the event's SSID names the network that is wanted. An event without
// an SSID cannot be attributed and counts as matching.
bool is_for_target(const uint8_t *event_ssid, size_t event_ssid_len,
                   const char *target_ssid, size_t target_ssid_len);

DisconnectAction on_disconnected(bool connect_requested, bool joining,
                                 bool for_target, unsigned retries,
                                 unsigned max_retries);

}  // namespace wifipolicy
