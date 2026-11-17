#pragma once

// The mock's camera identity (#122 stage 2): the MAC from NVS (console
// `id <MAC>`), else CONFIG_CAMERA_MOCK_DEV_ID. Read once at boot; BLE
// advertising and the device info use it.

#include <string>

#include "mock_camera.hpp"

namespace mock_identity
{

camera_mock::Identity load();
// Stores the MAC for the next start; false if it is not a valid MAC.
bool save(const std::string &mac);
// Back to CONFIG_CAMERA_MOCK_DEV_ID at the next start.
void clear();

}  // namespace mock_identity
