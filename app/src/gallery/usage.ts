import type { DataUsageDay } from './types';

/** Days the current consumption is averaged over: uploads come in bursts. */
export const USAGE_WINDOW_DAYS = 7;
const MB = 1_000_000;
const DAY_S = 86_400;

export interface UsageSummary {
  /** Bytes of the current calendar month so far (sent + received). */
  usedBytes: number;
  /** Bytes per day over the last days (at most 7, fewer while the data is younger). */
  perDayBytes: number;
  /** Days that rate is taken over. */
  windowDays: number;
  /** Expected bytes at the end of the month: used + rate × days left. */
  forecastBytes: number;
  /** The transports that carried the month's data, e.g. ['modem']. */
  transports: string[];
  /** The newest day with a value (UTC day, `YYYY-MM-DD`). */
  lastDay: string;
  /** With a budget: share used, 0-1+ (can pass 1). */
  fraction?: number;
  budgetBytes?: number;
  /** With a budget: Unix seconds when the plan runs out, if before the month ends. */
  runsOutAt?: number;
  /** The plan is used up already. */
  exhausted?: boolean;
}

/** `YYYY-MM` of a Unix time in the user's time zone. */
function monthKey(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}`;
}

/** A hub's UTC day as a point in time: its noon, so that the local date is the same. */
function dayNoon(day: string): Date {
  return new Date(`${day}T12:00:00Z`);
}

/** Sum of both directions. */
const total = (d: DataUsageDay): number => d.txBytes + d.rxBytes;

/**
 * Used volume of the current calendar month (user's time zone), consumption
 * over the last days, and a forecast for the end of the month; with a budget
 * the percentage and the day it runs out. `undefined` without any value.
 */
export function monthlyUsage(
  days: DataUsageDay[],
  nowSeconds: number,
  budgetMB?: number,
): UsageSummary | undefined {
  if (days.length === 0) return undefined;
  const now = new Date(nowSeconds * 1000);
  const month = monthKey(now);
  const inMonth = days.filter((d) => monthKey(dayNoon(d.day)) === month);
  const lastDay = days.reduce((a, d) => (d.day > a ? d.day : a), '');
  const firstDay = days.reduce((a, d) => (d.day < a ? d.day : a), days[0]?.day ?? '');
  const usedBytes = inMonth.reduce((sum, d) => sum + total(d), 0);

  // The rate: the last days, fewer while the data is younger than a week.
  const windowStart = nowSeconds - USAGE_WINDOW_DAYS * DAY_S;
  const dataDays = Math.ceil((nowSeconds - dayNoon(firstDay).getTime() / 1000) / DAY_S) + 1;
  const windowDays = Math.max(1, Math.min(USAGE_WINDOW_DAYS, dataDays));
  const recent = days
    .filter((d) => dayNoon(d.day).getTime() / 1000 >= windowStart)
    .reduce((sum, d) => sum + total(d), 0);
  const perDayBytes = recent / windowDays;

  const endOfMonth = new Date(now.getFullYear(), now.getMonth() + 1, 1).getTime() / 1000;
  const daysLeft = Math.max(0, (endOfMonth - nowSeconds) / DAY_S);
  const forecastBytes = usedBytes + perDayBytes * daysLeft;

  const summary: UsageSummary = {
    usedBytes,
    perDayBytes,
    windowDays,
    forecastBytes,
    transports: [...new Set(inMonth.filter((d) => total(d) > 0).map((d) => d.transport))].sort(),
    lastDay,
  };
  if (budgetMB !== undefined && budgetMB > 0) {
    const budgetBytes = budgetMB * MB;
    summary.budgetBytes = budgetBytes;
    summary.fraction = usedBytes / budgetBytes;
    if (usedBytes >= budgetBytes) {
      summary.exhausted = true;
    } else if (perDayBytes > 0 && forecastBytes > budgetBytes) {
      summary.runsOutAt = nowSeconds + ((budgetBytes - usedBytes) / perDayBytes) * DAY_S;
    }
  }
  return summary;
}
