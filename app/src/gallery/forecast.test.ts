import { describe, expect, it } from 'vitest';
import { batteryForecast } from './forecast';
import { forecastText } from './text';

const H = 3600;
const hourly = (percents: number[]) =>
  percents.map((batPercent, i) => ({ at: 1_000_000 + i * H, batPercent }));

/** Real values of a hub (Nov 2026): 100 % is read while charging. */
const REAL: [number, number][] = [
  [1794849997, 100],
  [1794855357, 100],
  [1794897225, 93],
  [1794901249, 85],
  [1794906651, 81],
  [1794910252, 79],
  [1794915613, 77],
  [1794919213, 75],
  [1794922814, 72],
  [1794926499, 68],
  [1795371025, 100],
  [1795374900, 58],
  [1795380303, 39],
  [1795385699, 24],
  [1795389300, 16],
  [1795392902, 12],
  [1795419628, 100],
  [1795423229, 100],
  [1795427201, 72],
  [1795432576, 69],
  [1795437953, 66],
];
const real = (until: number) =>
  REAL.filter(([at]) => at <= until).map(([at, batPercent]) => ({ at, batPercent }));

describe('battery forecast', () => {
  it('ignores 100 % values and everything before a charge (real data)', () => {
    // After the last charge: 72, 69, 66 in 3 h = 2 %/h, 66 % left = about 33 h.
    const f = batteryForecast(real(Infinity));
    expect(f?.drainPerHour).toBeCloseTo(2, 0);
    expect(f?.windowS).toBeLessThan(4 * H);
    expect((f?.emptyAt ?? 0) - (f?.lastAt ?? 0)).toBeGreaterThan(30 * H);
    expect((f?.emptyAt ?? 0) - (f?.lastAt ?? 0)).toBeLessThan(36 * H);
    // The run before: 58 to 12 % within 5 h, empty soon after the last value.
    const before = batteryForecast(real(1795392902));
    expect(before?.drainPerHour).toBeGreaterThan(8);
    expect((before?.emptyAt ?? 0) - (before?.lastAt ?? 0)).toBeLessThan(2 * H);
    // Latest value 100 %: charging or full, no forecast from older values.
    const charging = batteryForecast(real(1795423229));
    expect(charging?.atFull).toBe(true);
    expect(charging?.emptyAt).toBeUndefined();
    expect(charging && forecastText(charging, 1795423229)).toMatch(/Lädt oder ist voll/);
  });

  it('trusts the hub's charging flag over the 100 % rule (#421)', () => {
    const flagged = (rows: [number, number, boolean?][]) =>
      rows.map(([h, batPercent, charging]) => ({ at: 1_000_000 + h * H, batPercent, charging }));
    // A real 100 % at rest (flag false) is a value like any other.
    const rest = batteryForecast(flagged([[0, 100, false], [2, 98, false], [4, 96, false]]));
    expect(rest?.drainPerHour).toBeCloseTo(1, 5);
    // Charging below 100 %: dropped, and the run starts after it.
    const f = batteryForecast(
      flagged([[0, 80], [1, 78], [2, 90, true], [3, 95, true], [4, 70], [6, 66]]),
    );
    expect(f?.drainPerHour).toBeCloseTo(2, 5);
    expect(f?.windowS).toBe(2 * H);
    // Latest value flagged: charging, no forecast.
    const now = batteryForecast(flagged([[0, 80], [2, 78], [3, 90, true]]));
    expect(now?.charging).toBe(true);
    expect(now?.emptyAt).toBeUndefined();
    expect(now && forecastText(now, 1_000_000 + 3 * H)).toBe('Lädt, keine Prognose.');
  });

  it('forecasts from the first discharge after a charge once two values are an hour apart', () => {
    expect(batteryForecast(real(1794897225))).toBeUndefined(); // only 93 %
    const f = batteryForecast(real(1794901249)); // 93, 85 over 1.1 h
    expect(f?.drainPerHour).toBeGreaterThan(5);
  });

  it('shows the charge rate while the values rise, and starts over after it', () => {
    const rising = batteryForecast(hourly([40, 50, 60]));
    expect(rising?.drainPerHour).toBeCloseTo(-10, 5);
    expect(rising?.emptyAt).toBeUndefined();
    // Discharge, charge, discharge: only the last run counts.
    const samples = hourly([90, 80, 70, 90, 95, 94, 92, 90]);
    expect(batteryForecast(samples)?.drainPerHour).toBeCloseTo(1.7, 5);
  });

  it('treats a single point of gauge noise as no charge', () => {
    const f = batteryForecast(hourly([80, 79, 80, 78, 77]));
    expect(f?.drainPerHour).toBeGreaterThan(0.5);
    expect(f?.windowS).toBe(4 * H);
  });

  it('needs two values an hour apart', () => {
    expect(batteryForecast([])).toBeUndefined();
    expect(batteryForecast([{ at: 0, batPercent: 50 }])).toBeUndefined();
    expect(
      batteryForecast([
        { at: 0, batPercent: 50 },
        { at: 1800, batPercent: 49 },
      ]),
    ).toBeUndefined();
  });

  it('computes the drain and the time of an empty battery', () => {
    const samples = hourly([80, 79, 78, 77, 76]);
    const f = batteryForecast(samples);
    expect(f?.drainPerHour).toBeCloseTo(1, 5);
    expect(f?.emptyAt).toBeCloseTo(1_000_000 + 4 * H + 76 * H, 0);
  });

  it('uses only the last 24 hours', () => {
    const old = hourly([100, 100, 100, 100, 100]);
    const recent = Array.from({ length: 25 }, (_, i) => ({
      at: 1_000_000 + 100 * H + i * H,
      batPercent: 90 - i * 0.5,
    }));
    expect(batteryForecast([...old, ...recent])?.drainPerHour).toBeCloseTo(0.5, 5);
  });

  it('gives no empty time while charging or flat', () => {
    const charging = batteryForecast(hourly([50, 52, 54]));
    expect(charging?.drainPerHour).toBeCloseTo(-2, 5);
    expect(charging?.emptyAt).toBeUndefined();
    expect(batteryForecast(hourly([60, 60, 60]))?.emptyAt).toBeUndefined();
  });

  it('words the result in German', () => {
    const now = 1_000_000 + 4 * H;
    const f = batteryForecast(hourly([80, 79, 78, 77, 76]));
    expect(f && forecastText(f, now)).toMatch(
      /^Verbrauch: 1,0 %\/h \(letzte 4 h\)\. Leer voraussichtlich .* \(in 3 Tage/,
    );
    const c = batteryForecast(hourly([50, 52, 54]));
    expect(c && forecastText(c, now)).toBe('Lädt: +2,0 %/h (letzte 2 h), keine Prognose.');
    const dead = batteryForecast(hourly([5, 4, 3]));
    expect(dead && forecastText(dead, 1_000_000 + 100 * H)).toMatch(
      /Laut Prognose war der Akku .* leer\. Letzter Wert von /,
    );
    // A latest value from days ago says so, the forecast stays measured from it.
    const stale = batteryForecast(hourly([100, 99, 98]));
    expect(stale && forecastText(stale, 1_000_000 + 2 * H + 30 * H)).toMatch(
      /Leer voraussichtlich .* \(in 3 Tage\)\. Letzter Wert von /,
    );
  });
});
