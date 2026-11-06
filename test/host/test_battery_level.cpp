#include <doctest/doctest.h>

#include "battery_level.hpp"

using namespace hub_battery;

TEST_CASE("MAX17048 version register")
{
    CHECK(is_max17048_version(0x0010));
    CHECK(is_max17048_version(0x0012));
    CHECK(is_max17048_version(0x001F));
    CHECK_FALSE(is_max17048_version(0x0000));
    CHECK_FALSE(is_max17048_version(0x0020));
    CHECK_FALSE(is_max17048_version(0xFFFF));
}

TEST_CASE("VCELL is 78.125 uV per bit")
{
    CHECK(vcell_to_millivolts(0) == 0);
    // 4.2 V = 53760 * 78.125 uV
    CHECK(vcell_to_millivolts(53760) == 4200);
    // 3.7 V = 47360 * 78.125 uV
    CHECK(vcell_to_millivolts(47360) == 3700);
    // Rounded to the nearest millivolt.
    CHECK(vcell_to_millivolts(47366) == 3700);
    CHECK(vcell_to_millivolts(47367) == 3701);
    CHECK(vcell_to_millivolts(0xFFFF) == 5120);
}

TEST_CASE("SOC is 1/256 percent per bit, rounded and clamped")
{
    CHECK(soc_to_percent(0) == 0);
    CHECK(soc_to_percent(82 * 256) == 82);
    CHECK(soc_to_percent(82 * 256 + 127) == 82);
    CHECK(soc_to_percent(82 * 256 + 128) == 83);
    CHECK(soc_to_percent(100 * 256) == 100);
    // A full cell reads slightly above 100 %.
    CHECK(soc_to_percent(103 * 256) == 100);
    CHECK(soc_to_percent(0xFFFF) == 100);
}

TEST_CASE("Plausible cell voltages")
{
    CHECK(plausible_cell_millivolts(3700));
    CHECK(plausible_cell_millivolts(2500));
    CHECK(plausible_cell_millivolts(4500));
    CHECK_FALSE(plausible_cell_millivolts(0));
    CHECK_FALSE(plausible_cell_millivolts(2499));
    CHECK_FALSE(plausible_cell_millivolts(5120));
}

TEST_CASE("Voltage to percent along the Li-ion curve")
{
    SUBCASE("ends are clamped")
    {
        CHECK(voltage_to_percent(3300, 3300, 4200) == 0);
        CHECK(voltage_to_percent(3000, 3300, 4200) == 0);
        CHECK(voltage_to_percent(4200, 3300, 4200) == 100);
        CHECK(voltage_to_percent(4350, 3300, 4200) == 100);
    }
    SUBCASE("curve points")
    {
        CHECK(voltage_to_percent(3500, 3300, 4200) == 5);
        CHECK(voltage_to_percent(3700, 3300, 4200) == 30);
        CHECK(voltage_to_percent(3800, 3300, 4200) == 50);
        CHECK(voltage_to_percent(4100, 3300, 4200) == 90);
    }
    SUBCASE("between points: linear and rounded")
    {
        CHECK(voltage_to_percent(3725, 3300, 4200) == 35);
        CHECK(voltage_to_percent(3400, 3300, 4200) == 3);  // 2.5 rounds up
        CHECK(voltage_to_percent(4150, 3300, 4200) == 95);
    }
    SUBCASE("monotonic")
    {
        int last = -1;
        for (int mv = 3200; mv <= 4300; mv += 5) {
            const int percent = voltage_to_percent(mv, 3300, 4200);
            CHECK(percent >= last);
            last = percent;
        }
    }
    SUBCASE("configured ends move the curve")
    {
        // Full at 4100 mV: 4100 is 100 %, the middle of 3300..4100 is the
        // middle of the curve (3750 mV = 40 %).
        CHECK(voltage_to_percent(4100, 3300, 4100) == 100);
        CHECK(voltage_to_percent(3700, 3300, 4100) == 40);
        // Empty at 3500 mV.
        CHECK(voltage_to_percent(3500, 3500, 4200) == 0);
        CHECK(voltage_to_percent(3400, 3500, 4200) == 0);
    }
    SUBCASE("invalid range")
    {
        CHECK(voltage_to_percent(3700, 4200, 4200) == -1);
        CHECK(voltage_to_percent(3700, 4200, 3300) == -1);
    }
}
