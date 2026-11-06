#pragma once

// Battery level of the hub: decoding the registers of the MAX17048 fuel gauge
// and mapping a cell voltage to a percentage. Pure logic, host-testable; the
// I2C access lives in hub_battery.cpp.

#include <cstdint>

namespace hub_battery
{

// MAX17048 (7-bit I2C address 0x36). Registers are 16 bits, MSB first.
constexpr std::uint8_t kMax17048Address = 0x36;
constexpr std::uint8_t kRegVcell = 0x02;
constexpr std::uint8_t kRegSoc = 0x04;
constexpr std::uint8_t kRegVersion = 0x08;

// VERSION reads 0x001X on the MAX17048/MAX17049.
bool is_max17048_version(std::uint16_t version);

// VCELL: 78.125 uV per bit.
int vcell_to_millivolts(std::uint16_t raw);

// SOC: 1/256 % per bit, rounded and clamped to 0..100. The gauge reports
// slightly above 100 % on a full cell.
int soc_to_percent(std::uint16_t raw);

// A single Li-ion cell (or the gauge reading one) lies in this range; outside
// it the reading is not trusted (no cell, bus error).
bool plausible_cell_millivolts(int millivolts);

// Percentage of a single Li-ion cell from its voltage, along a typical
// discharge curve from 3300 mV (0 %) to 4200 mV (100 %). `empty_mv` and
// `full_mv` move the ends of the curve: the voltage is first scaled linearly
// from empty..full onto 3300..4200. Clamped to 0..100; -1 if full_mv is not
// above empty_mv.
int voltage_to_percent(int millivolts, int empty_mv, int full_mv);

}  // namespace hub_battery
