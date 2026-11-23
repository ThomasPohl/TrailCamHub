import { describe, expect, it } from 'vitest';
import { usageLines, megabyteText } from './text';
import type { DataUsageDay } from './types';
import { monthlyUsage } from './usage';

/** Local noon on a day, as Unix seconds (the tests do not depend on the time zone). */
const at = (year: number, month: number, day: number) =>
  new Date(year, month - 1, day, 12).getTime() / 1000;

const day = (d: string, tx: number, rx = 0): DataUsageDay => ({
  day: d,
  transport: 'modem',
  txBytes: tx,
  rxBytes: rx,
});

describe('monthlyUsage', () => {
  it('has nothing without a value', () => {
    expect(monthlyUsage([], at(2026, 11, 15))).toBeUndefined();
    expect(usageLines(undefined, at(2026, 11, 15))).toEqual(['Noch keine Daten zur Datenmenge.']);
  });

  it('adds up the current month only and forecasts to its end', () => {
    // 15 Nov 2026 at noon: 15.5 days left in a 30-day month.
    const now = at(2026, 11, 15);
    const days = [
      day('2026-10-30', 900_000_000), // last month: not counted
      day('2026-11-09', 7_000_000),
      day('2026-11-14', 10_000_000, 4_000_000),
    ];
    const u = monthlyUsage(days, now);
    expect(u?.usedBytes).toBe(21_000_000);
    // 7 days: the 9th and the 14th are in the window, 21 MB / 7.
    expect(u?.windowDays).toBe(7);
    expect(u?.perDayBytes).toBeCloseTo(3_000_000, -3);
    expect(u?.forecastBytes).toBeCloseTo(21_000_000 + 3_000_000 * 15.5, -4);
    expect(u?.transports).toEqual(['modem']);
    expect(u?.fraction).toBeUndefined();
  });

  it('averages over fewer days while the data is young', () => {
    const now = at(2026, 11, 15);
    const u = monthlyUsage([day('2026-11-14', 6_000_000), day('2026-11-15', 6_000_000)], now);
    expect(u?.windowDays).toBeLessThan(7);
    expect(u?.perDayBytes).toBeGreaterThan(3_000_000);
  });

  it('with a budget: percentage and the day it runs out', () => {
    const now = at(2026, 11, 15);
    const days = [day('2026-11-13', 50_000_000), day('2026-11-14', 50_000_000)];
    const u = monthlyUsage(days, now, 500);
    expect(u?.fraction).toBeCloseTo(0.2);
    // 100 MB over 7 days is ~14 MB/day: 400 MB left last ~28 days, longer than the month.
    expect(u?.runsOutAt).toBeUndefined();
    const tight = monthlyUsage(days, now, 150);
    expect(tight?.runsOutAt).toBeGreaterThan(now);
    expect(tight?.runsOutAt).toBeLessThan(at(2026, 12, 1));
    const done = monthlyUsage(days, now, 80);
    expect(done?.exhausted).toBe(true);
    expect(done?.runsOutAt).toBeUndefined();
  });

  it('starts over with the new month', () => {
    const days = [day('2026-11-28', 40_000_000), day('2026-11-29', 40_000_000)];
    const dec = monthlyUsage(days, at(2026, 12, 2));
    expect(dec?.usedBytes).toBe(0);
    expect(dec?.lastDay).toBe('2026-11-29');
    // The last week still sets the rate.
    expect(dec?.perDayBytes).toBeGreaterThan(0);
  });
});

describe('usage text', () => {
  it('writes plain German with and without a budget', () => {
    const now = at(2026, 11, 15);
    const days = [day('2026-11-13', 50_000_000), day('2026-11-14', 50_000_000)];
    const free = usageLines(monthlyUsage(days, now), now);
    expect(free[0]).toBe('Diesen Monat: 100 MB (Mobilfunk).');
    expect(free.some((l) => l.startsWith('Bis Monatsende voraussichtlich'))).toBe(true);
    const planned = usageLines(monthlyUsage(days, now, 150), now);
    expect(planned.some((l) => l.startsWith('67 % von 150 MB verbraucht.'))).toBe(true);
    expect(planned.some((l) => l.startsWith('Reicht voraussichtlich nur bis '))).toBe(true);
    const used = usageLines(monthlyUsage(days, now, 80), now);
    expect(used).toContain('Das Kontingent ist aufgebraucht.');
  });

  it('formats megabytes', () => {
    expect(megabyteText(400_000)).toBe('0,4 MB');
    expect(megabyteText(12_300_000)).toBe('12 MB');
    expect(megabyteText(1_340_000_000)).toBe('1,3 GB');
  });
});
