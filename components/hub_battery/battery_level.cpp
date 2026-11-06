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

}  // namespace hub_battery
