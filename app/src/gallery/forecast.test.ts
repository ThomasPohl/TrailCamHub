import { describe, expect, it } from 'vitest';
import { batteryForecast } from './forecast';
import { forecastText } from './text';

const H = 3600;
const hourly = (percents: number[]) =>
  percents.map((batPercent, i) => ({ at: 1_000_000 + i * H, batPercent }));

describe('battery forecast', () => {
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
