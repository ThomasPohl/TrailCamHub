#pragma once

// The hub's own battery. On the Waveshare ESP32-S3-A7670E-4G a MAX17048 fuel
// gauge on I2C measures the cell (CONFIG_HUB_BATTERY_MAX17048); other boards
// have no measurement and report "unknown".
//
// Thread-safe: the console, the uplink and the memory log read it.

namespace hub_battery
{

struct Reading {
    int millivolts = -1;     // cell voltage; -1 = unknown
    int percent = -1;        // the percentage that is reported; -1 = unknown
    int gauge_percent = -1;  // the gauge's own state of charge; -1 = unknown
};

// True when the board measures its battery (Kconfig).
bool supported();

// Name of the measurement, e.g. "MAX17048"; "none" without one.
const char *source_name();

// Reads the gauge now. False when the board has no measurement or the gauge
// does not answer (for example no cell inserted); `out` is then "unknown".
bool read(Reading &out);

// The battery level in percent, or -1 if unknown.
int percent();

// Writes the reading to the log, with `stage` naming the occasion.
void log_reading(const char *stage);

}  // namespace hub_battery
