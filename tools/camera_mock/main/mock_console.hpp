#pragma once

// Serial console of the camera mock (UART, 115200 baud): status, close,
// ap on|off.

#include "esp_err.h"

namespace mock_console
{

esp_err_t start();

}  // namespace mock_console
