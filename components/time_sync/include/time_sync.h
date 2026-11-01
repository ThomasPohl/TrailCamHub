#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "time_policy.hpp"

/*
 * The clock of the device is not set at boot, and the times that the firmware
 * records (a camera's last sync, file times) and sends (the camera login)
 * need it. It can be set from several sources, in this order of preference
 * (see time_policy.hpp): SNTP, the backend, a browser that opens the web UI,
 * the camera. A better source replaces a worse one; a worse one sets the
 * clock only while it holds no time at all.
 */

/*
 * Starts the SNTP client, which sets the clock as soon as a network reaches
 * an NTP server (CONFIG_TIME_SYNC_SERVER) and keeps it up to date. Needs
 * esp_netif_init() and the default event loop. Does nothing if
 * CONFIG_TIME_SYNC_SNTP is off.
 */
esp_err_t time_sync_start(void);

/* True once the clock holds a plausible time (2024 or later). */
bool time_sync_clock_valid(void);

/* The source of the time that the clock holds (none: not set by this module).
 */
timepolicy::Source time_sync_source(void);

/*
 * True if the clock holds a time that is good enough to give to the camera
 * (from SNTP, the backend or a browser, but not from the camera itself).
 */
bool time_sync_trusted(void);

/*
 * Sets the clock to `unix_seconds` (UTC) if the time is plausible, the source
 * is enabled in the configuration and it is not worse than the source of the
 * time that the clock holds. Returns whether the clock was set.
 */
bool time_sync_set(int64_t unix_seconds, timepolicy::Source source);
