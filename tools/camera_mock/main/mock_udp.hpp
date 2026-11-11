#pragma once

// The camera's UDP endpoint (port 40611): one task that feeds received
// datagrams into camera_mock::Camera and runs its timers.

#include <string>

#include "esp_err.h"

namespace mock_udp
{

esp_err_t start();
// One line per counter, for the console.
std::string status();
// Ends the session from the camera side (CLOSE).
void close_session();

}  // namespace mock_udp
