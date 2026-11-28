#pragma once
#include "esp_err.h"
// Host: no TLS; the attach function is only passed along.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);
