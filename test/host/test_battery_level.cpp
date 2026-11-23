#include <doctest/doctest.h>

#include <string>

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

TEST_CASE("MAX17048 charge rate and charging")
{
    CHECK(crate_to_millipercent_per_hour(0) == 0);
    CHECK(crate_to_millipercent_per_hour(5) == 1040);
    CHECK(crate_to_millipercent_per_hour(0xFFFB) == -1040);  // -5
    CHECK(is_charging(crate_to_millipercent_per_hour(3)));
    CHECK_FALSE(is_charging(crate_to_millipercent_per_hour(2)));
    CHECK_FALSE(is_charging(crate_to_millipercent_per_hour(0xFFF0)));
}

TEST_CASE("the modem needs enough cell voltage")
{
    CHECK(modem_allowed(3400, 3400));
    CHECK(modem_allowed(3950, 3400));
    CHECK_FALSE(modem_allowed(3399, 3400));
    // Unknown voltage or no limit: allowed.
    CHECK(modem_allowed(-1, 3400));
    CHECK(modem_allowed(3000, 0));
}

TEST_CASE("gauge failure hint names the I2C bus on a timeout")
{
    CHECK(std::string(gauge_failure_hint(true)).find("CAM DIP switch") !=
          std::string::npos);
    CHECK(std::string(gauge_failure_hint(false)).find("no cell inserted") !=
          std::string::npos);
}

TEST_CASE("the gauge is retried at once three times, then with back-off (#412)")
{
    CHECK(gauge_retry_delay_ms(0) == 0);
    CHECK(gauge_retry_delay_ms(2) == 0);
    CHECK(gauge_retry_delay_ms(3) == 2000);
    CHECK(gauge_retry_delay_ms(4) == 4000);
    CHECK(gauge_retry_delay_ms(5) == 8000);
    CHECK(gauge_retry_delay_ms(30) == kGaugeBackoffMaxMs);
    CHECK(gauge_retry_delay_ms(1000000) == kGaugeBackoffMaxMs);
}

TEST_CASE("the I2C bus is reset only after timeouts past the free attempts")
{
    CHECK_FALSE(gauge_should_reset_bus(2, true));
    CHECK(gauge_should_reset_bus(3, true));
    CHECK_FALSE(gauge_should_reset_bus(10, false));
}

TEST_CASE("a cell is only lost after several boots without an answer (#417)")
{
    int misses = 0;
    // Answers: present, counter reset.
    CHECK(cell_present(true, false, misses));
    CHECK(misses == 0);
    // Never seen: stays absent.
    CHECK_FALSE(cell_present(false, false, misses));
    // Seen before: two silent boots keep it, the third loses it.
    CHECK(cell_present(false, true, misses));
    CHECK(misses == 1);
    CHECK(cell_present(false, true, misses));
    CHECK_FALSE(cell_present(false, true, misses));
    // An answer in between starts over.
    misses = 2;
    CHECK(cell_present(true, true, misses));
    CHECK(misses == 0);
    CHECK(cell_present(false, true, misses));
}

TEST_CASE("a detected battery with power mains is flagged")
{
    CHECK(power_mode_mismatch(true, 3900));
    CHECK_FALSE(power_mode_mismatch(false, 3900));  // battery mode: fine
    CHECK_FALSE(power_mode_mismatch(true, -1));     // no gauge answer
    CHECK_FALSE(power_mode_mismatch(true, 0));      // implausible voltage
    CHECK_FALSE(power_mode_mismatch(true, 6000));
    CHECK(std::string(power_mode_mismatch_text()).find("power=mains") !=
          std::string::npos);
}
