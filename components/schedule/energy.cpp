#include "energy.hpp"

#include <cstdlib>

namespace schedule
{

namespace
{

// A reading further than this after the previous one adds no full time (the
// hub slept or a reading is missing).
constexpr std::int64_t kMaxGapS = 2 * 3600;

void push_day(EnergyHistory &history, int morning)
{
    for (std::size_t i = kEnergyDays - 1; i > 0; i--) {
        history.days[i] = history.days[i - 1];
    }
    history.days[0] = EnergyDay();
    history.days[0].morning = static_cast<std::int16_t>(morning);
    if (history.count < kEnergyDays) {
        history.count++;
    }
}

int balance(const EnergyHistory &history, std::size_t i)
{
    return history.days[i].morning - history.days[i + 1].morning;
}

bool trend_of(const EnergyHistory &history, int &trend)
{
    if (history.count < 2) {
        return false;
    }
    int sum = 0;
    int n = 0;
    for (std::size_t i = 0; i < 3 && i + 1 < history.count; i++) {
        if (history.days[i].morning < 0 || history.days[i + 1].morning < 0) {
            break;
        }
        sum += balance(history, i);
        n++;
    }
    if (n == 0) {
        return false;
    }
    // Rounded to the nearest, away from zero for halves.
    trend = (sum * 2 + (sum >= 0 ? n : -n)) / (2 * n);
    return true;
}

// The step after a new morning record.
void update_step(EnergyHistory &history, const EnergySettings &settings)
{
    if (history.count < settings.min_days || history.days[0].morning < 0) {
        history.step = 0;
        return;
    }
    const int morning = history.days[0].morning;
    int trend = 0;
    if (!trend_of(history, trend)) {
        history.step = 0;
        return;
    }
    const bool lost =
        history.days[1].full_minutes > (std::uint16_t)settings.full_minutes;
    if (morning < settings.target_percent || trend < -settings.drop_percent) {
        if (history.step > 0) {
            history.step--;
        }
    } else if ((morning >= settings.target_percent + settings.margin_percent &&
                trend >= 0) ||
               lost) {
        if (history.step < kEnergyMaxStep) {
            history.step++;
        }
    }
}

}  // namespace

bool energy_reading(EnergyHistory &history, EnergyToday &today, int date,
                    std::int64_t now, int percent, bool charging,
                    const EnergySettings &settings)
{
    if (percent < 0 || date <= 0) {
        return false;
    }
    bool changed = false;
    if (today.date != date) {
        const bool continues =
            today.date != 0 && history.last_date == today.date;
        if (history.last_date != date) {
            // The first reading of a new day: the morning record.
            if (continues && history.count > 0) {
                const std::uint32_t minutes = today.full_seconds / 60;
                history.days[0].full_minutes =
                    minutes > 0xFFFF ? 0xFFFF
                                     : static_cast<std::uint16_t>(minutes);
            }
            if (history.count > 0 && history.days[0].morning >= 0 &&
                std::abs(percent - history.days[0].morning) >
                    settings.cell_change_percent) {
                history = EnergyHistory();
            }
            push_day(history, percent);
            history.last_date = date;
            update_step(history, settings);
            changed = true;
        }
        today.date = date;
        today.last_reading = 0;
        today.last_saturated = false;
        today.full_seconds = 0;
    }
    // An interval counts when both of its readings are full and charging.
    const bool saturated = charging && percent >= settings.saturated_percent;
    if (saturated && today.last_saturated && today.last_reading > 0 &&
        now > today.last_reading && now - today.last_reading <= kMaxGapS) {
        today.full_seconds +=
            static_cast<std::uint32_t>(now - today.last_reading);
    }
    today.last_saturated = saturated;
    today.last_reading = now;
    return changed;
}

std::uint16_t energy_permille(const EnergyHistory &history, int percent,
                              const EnergySettings &settings)
{
    std::uint16_t permille = static_cast<std::uint16_t>(
        1000 -
        250 * (history.step > kEnergyMaxStep ? kEnergyMaxStep : history.step));
    if (percent >= settings.full_percent && permille > 500) {
        permille = 500;
    }
    return permille;
}

bool energy_trend(const EnergyHistory &history, int &trend_percent)
{
    return trend_of(history, trend_percent);
}

}  // namespace schedule
