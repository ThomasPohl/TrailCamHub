#pragma once

// The camera's Wi-Fi access point: WPA2, SSID "TrailCam_XXXX" from the last
// two bytes of the BSSID, address 192.168.43.1 with a DHCP server, as on the
// real camera.

#include <string>

#include "esp_err.h"

namespace mock_ap
{

// Creates the AP interface and initialises Wi-Fi (not started).
esp_err_t init();
// Starts the access point and waits until it is up. True if it is running.
bool start();
void stop();
bool running();

std::string ssid();
// BSSID in the camera's format, "1C:4E:A2:7E:5D:BD".
std::string bssid();
std::string password();
// Stations connected to the access point.
int station_count();

}  // namespace mock_ap
