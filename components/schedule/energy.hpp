#pragma once

// Energy factor of the check-in interval from the hub's own battery history
// (#436). Pure logic, host-testable.
//
// The hub keeps one record per local day: the charge in the morning (the first
// reading of the day, at rest) and how long the cell sat full while the sun
// still charged it (energy that was lost). From the last days it derives a
// factor for the check-in interval in steps of 0.25 between 0.5 and 1: a cell
// that is well charged in the morning and a trend that is not falling mean the
// hub can wake more often; a low morning or a falling trend take it back.
// A full cell halves the interval at once, for the rest of the day.

#include <cstddef>
#include <cstdint>

namespace schedule
{

constexpr std::size_t kEnergyDays = 7;
// Check-in interval at the most stretched step: 1.0, 0.75, 0.5.
constexpr std::uint8_t kEnergyMaxStep = 2;

struct EnergyDay {
    // Charge in the morning (%); -1: unknown.
    std::int16_t morning = -1;
    // Minutes the cell was full (and charging) during this day.
    std::uint16_t full_minutes = 0;
};

struct EnergySettings {
    // The charge the cell should have in the morning.
    int target_percent = 60;
    // Charge above the target (and a trend that is not falling) that allows a
    // step towards more frequent check-ins.
    int margin_percent = 15;
    // A trend falling faster than this (% per day) takes a step back.
    int drop_percent = 5;
    // Minutes at "full" on the day before that allow a step as well.
    int full_minutes = 30;
    // From this charge on the interval is halved at once.
    int full_percent = 95;
    // A reading counts as full from this charge, while charging.
    int saturated_percent = 98;
    // A jump of the morning charge by more than this between two days means a
    // new cell: the history starts again.
    int cell_change_percent = 30;
    // Days of history before the factor leaves 1.
    int min_days = 2;
};

// Stored in NVS (a blob), written once a day: plain fields only.
struct EnergyHistory {
    std::uint32_t version = 1;
    // Local date (yyyymmdd) of the newest record; 0: none.
    std::int32_t last_date = 0;
    std::uint8_t step = 0;
    std::uint8_t count = 0;
    std::uint8_t reserved[2] = {0, 0};
    // days[0] is the newest.
    EnergyDay days[kEnergyDays];
};

// Kept in RTC memory (survives deep sleep): what the running day collects.
struct EnergyToday {
    std::int32_t date;
    // Unix time of the last reading; 0: none.
    std::int64_t last_reading;
    // The last reading was full and charging.
    bool last_saturated;
    // Seconds at "full" so far today.
    std::uint32_t full_seconds;
};

// One reading at rest (`percent` -1: unknown). `date` is the local date as
// yyyymmdd, `now` the Unix time. Returns true if `history` changed and has to
// be stored.
bool energy_reading(EnergyHistory &history, EnergyToday &today, int date,
                    std::int64_t now, int percent, bool charging,
                    const EnergySettings &settings);

// Factor for the check-in interval in 1/1000 (1000, 750 or 500) for the
// current charge (-1: unknown). Never below 500.
std::uint16_t energy_permille(const EnergyHistory &history, int percent,
                              const EnergySettings &settings);

// The trend of the morning charge in % per day (mean of the last three
// balances, rounded); false if there are fewer than two days.
bool energy_trend(const EnergyHistory &history, int &trend_percent);

}  // namespace schedule
