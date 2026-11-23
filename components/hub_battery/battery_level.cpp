#include "battery_level.hpp"

#include <cstddef>

namespace hub_battery
{

namespace
{

constexpr int kCurveEmptyMv = 3300;
constexpr int kCurveFullMv = 4200;

struct CurvePoint {
    int millivolts;
    int percent;
};

// Resting voltage of a typical Li-ion cell (18650, LiPo) against its charge.
constexpr CurvePoint kCurve[] = {
    {3300, 0},  {3500, 5},  {3600, 12}, {3650, 20}, {3700, 30}, {3750, 40},
    {3800, 50}, {3870, 60}, {3950, 70}, {4020, 80}, {4100, 90}, {4200, 100},
};
constexpr std::size_t kCurvePoints = sizeof(kCurve) / sizeof(kCurve[0]);

}  // namespace

bool is_max17048_version(std::uint16_t version)
{
    return (version & 0xFFF0u) == 0x0010u;
}

int vcell_to_millivolts(std::uint16_t raw)
{
    // 78.125 uV = 5/64 mV; rounded.
    return (int)(((std::uint32_t)raw * 5u + 32u) / 64u);
}

int soc_to_percent(std::uint16_t raw)
{
    const int percent = (int)(((std::uint32_t)raw + 128u) / 256u);
    return percent > 100 ? 100 : percent;
}

bool plausible_cell_millivolts(int millivolts)
{
    return millivolts >= 2500 && millivolts <= 4500;
}

int voltage_to_percent(int millivolts, int empty_mv, int full_mv)
{
    if (full_mv <= empty_mv) {
        return -1;
    }
    // Scale empty..full onto the curve's range.
    const long long scaled =
        kCurveEmptyMv + (long long)(millivolts - empty_mv) *
                            (kCurveFullMv - kCurveEmptyMv) /
                            (full_mv - empty_mv);
    if (scaled <= kCurve[0].millivolts) {
        return 0;
    }
    if (scaled >= kCurve[kCurvePoints - 1].millivolts) {
        return 100;
    }
    for (std::size_t i = 1; i < kCurvePoints; ++i) {
        const CurvePoint &low = kCurve[i - 1];
        const CurvePoint &high = kCurve[i];
        if (scaled <= high.millivolts) {
            const long long span = high.millivolts - low.millivolts;
            const long long offset = scaled - low.millivolts;
            return low.percent +
                   (int)((offset * (high.percent - low.percent) + span / 2) /
                         span);
        }
    }
    return 100;
}

int crate_to_millipercent_per_hour(std::uint16_t raw)
{
    return (int)(std::int16_t)raw * 208;
}

bool is_charging(int millipercent_per_hour)
{
    return millipercent_per_hour >= 500;
}

bool modem_allowed(int millivolts, int min_mv)
{
    return millivolts < 0 || min_mv <= 0 || millivolts >= min_mv;
}

const char *gauge_failure_hint(bool bus_timeout)
{
    return bus_timeout ? "Battery gauge: the I2C bus does not respond "
                         "(timeout). Check that the CAM DIP switch is ON, the "
                         "pull-ups on SDA/SCL, and the cell"
                       : "Battery gauge does not answer (no cell inserted?)";
}

bool cell_present(bool answered, bool was_present, int &misses)
{
    if (answered) {
        misses = 0;
        return true;
    }
    if (!was_present) {
        misses = 0;
        return false;
    }
    if (misses < kMissesToLoseCell) {
        ++misses;
    }
    return misses < kMissesToLoseCell;
}

bool power_mode_mismatch(bool mains, int gauge_millivolts)
{
    return mains && plausible_cell_millivolts(gauge_millivolts);
}

const char *power_mode_mismatch_text()
{
    return "battery detected but power=mains: no deep sleep, the battery "
           "drains fast (set power to battery)";
}

}  // namespace hub_battery
