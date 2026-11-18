#pragma once

// The camera's UDP endpoint (port 40611): one task that feeds received
// datagrams into camera_mock::Camera and runs its timers.

#include <string>

#include "mock_camera.hpp"

#include "esp_err.h"

namespace mock_udp
{

esp_err_t start(const camera_mock::Identity &identity);
// One line per counter, for the console.
std::string status();
// Ends the session from the camera side (CLOSE).
void close_session();
camera_mock::Identity identity();
// The faults the mock injects (#122 stage 2); see camera_mock::Faults.
camera_mock::Faults faults();
void set_faults(const camera_mock::Faults &faults);
// Recordings in the media list (#122); see camera_mock::Options.
size_t media_count();
void set_media_count(size_t count);

}  // namespace mock_udp
