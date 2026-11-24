#include <doctest/doctest.h>

#include "energy.hpp"

using namespace schedule;

namespace
{

constexpr std::int64_t kDay = 86400;

struct Hub {
    EnergyHistory history;
    EnergyToday today = {};
    EnergySettings settings;
    int day = 0;

    // The first reading of a new day.
    bool morning(int percent, bool charging = false)
    {
        day++;
        return energy_reading(history, today, 20260101 + day,
                              day * kDay + 6 * 3600, percent, charging,
                              settings);
    }

    // A reading `minutes` after the morning of the current day.
    void reading(int minutes, int percent, bool charging)
    {
        energy_reading(history, today, 20260101 + day,
                       day * kDay + 6 * 3600 + minutes * 60LL, percent,
                       charging, settings);
    }

    std::uint16_t permille(int percent = 50) const
    {
        return energy_permille(history, percent, settings);
    }
};

}  // namespace

TEST_CASE("the factor stays 1 until two days are recorded")
{
    Hub hub;
    CHECK(hub.permille() == 1000);
    CHECK(hub.morning(90));
    CHECK(hub.history.count == 1);
    CHECK(hub.permille() == 1000);
    CHECK(hub.morning(92));
    // A good morning and a rising trend: one step.
    CHECK(hub.permille() == 750);
}

TEST_CASE("a well charged cell with a steady trend wakes more often")
{
    Hub hub;
    hub.morning(80);
    hub.morning(82);
    CHECK(hub.permille() == 750);
    hub.morning(83);
    CHECK(hub.permille() == 500);
    // The most frequent step is 0.5.
    hub.morning(85);
    CHECK(hub.permille() == 500);
}

TEST_CASE("a low morning or a falling trend take a step back")
{
    Hub hub;
    hub.morning(76);
    hub.morning(80);
    hub.morning(85);
    REQUIRE(hub.permille() == 500);
    // Below the target of 60 %.
    hub.morning(58);
    CHECK(hub.permille() == 750);
    hub.morning(52);
    CHECK(hub.permille() == 1000);
    hub.morning(45);
    CHECK(hub.permille() == 1000);

    // A trend that falls more than 5 % a day, although above the target.
    Hub falling;
    falling.morning(95);
    falling.morning(96);
    falling.morning(97);
    REQUIRE(falling.permille() == 500);
    falling.morning(76);  // -21, mean of the last balances -6
    CHECK(falling.permille() == 750);
}

TEST_CASE("a mediocre morning leaves the step as it is")
{
    Hub hub;
    hub.morning(65);
    hub.morning(66);
    // Above the target but not by the margin, trend not falling.
    CHECK(hub.permille() == 1000);
    hub.morning(85);
    hub.morning(86);
    CHECK(hub.permille() == 500);
    hub.morning(70);  // above the target, below the margin, trend -16 (mean 6)
    const std::uint16_t after = hub.permille();
    CHECK(after >= 500);
}

TEST_CASE("time spent full while charging allows a step")
{
    Hub hub;
    hub.morning(62);
    // Day 1: full for an hour while charging.
    hub.reading(300, 98, true);
    hub.reading(330, 99, true);
    hub.reading(360, 99, true);
    hub.morning(62);
    // The morning is under target + margin, but the full time counts.
    CHECK(hub.history.days[1].full_minutes == 60);
    CHECK(hub.permille() == 750);
}

TEST_CASE("readings far apart or not charging add no full time")
{
    Hub hub;
    hub.morning(70);
    hub.reading(300, 99, true);
    hub.reading(600, 99, true);   // 5 h later: a gap, not counted
    hub.reading(630, 99, false);  // not charging: not full
    hub.reading(660, 99, false);
    hub.morning(70);
    CHECK(hub.history.days[1].full_minutes == 0);
}

TEST_CASE("a full cell halves the interval at once")
{
    Hub hub;
    CHECK(hub.permille(94) == 1000);
    CHECK(hub.permille(95) == 500);
    CHECK(hub.permille(100) == 500);
    CHECK(hub.permille(-1) == 1000);
}

TEST_CASE("a jump of the morning charge means a new cell")
{
    Hub hub;
    hub.morning(85);
    hub.morning(88);
    hub.morning(90);
    REQUIRE(hub.permille() == 500);
    hub.morning(40);  // 50 % lower: history starts again
    CHECK(hub.history.count == 1);
    CHECK(hub.permille() == 1000);
}

TEST_CASE("the history keeps seven days and the date is recorded once")
{
    Hub hub;
    for (int i = 0; i < 10; i++) {
        hub.morning(70);
    }
    CHECK(hub.history.count == kEnergyDays);
    // A second reading of the same day is no new record.
    CHECK_FALSE(energy_reading(hub.history, hub.today, 20260101 + hub.day,
                               hub.day * kDay + 9 * 3600, 71, false,
                               hub.settings));
    CHECK(hub.history.count == kEnergyDays);
    // After a reset the day is known from the history.
    EnergyToday fresh = {};
    CHECK_FALSE(energy_reading(hub.history, fresh, 20260101 + hub.day,
                               hub.day * kDay + 10 * 3600, 72, false,
                               hub.settings));
    CHECK(fresh.date == 20260101 + hub.day);
    CHECK(hub.history.count == kEnergyDays);
}

TEST_CASE("unknown charge records nothing")
{
    Hub hub;
    CHECK_FALSE(hub.morning(-1));
    CHECK(hub.history.count == 0);
    CHECK_FALSE(
        energy_reading(hub.history, hub.today, 0, 0, 80, false, hub.settings));
}

TEST_CASE("the trend is the mean of the last balances")
{
    Hub hub;
    int trend = 0;
    CHECK_FALSE(energy_trend(hub.history, trend));
    hub.morning(60);
    CHECK_FALSE(energy_trend(hub.history, trend));
    hub.morning(70);
    REQUIRE(energy_trend(hub.history, trend));
    CHECK(trend == 10);
    hub.morning(66);
    hub.morning(62);
    REQUIRE(energy_trend(hub.history, trend));
    // The last three balances: -4, -4 and +10; mean 0.67, rounded to 1.
    CHECK(trend == 1);
}
