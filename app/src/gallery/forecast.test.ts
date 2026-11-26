import { describe, expect, it } from 'vitest';
import { batteryForecast, batteryLevel, reportsLevels } from './forecast';
import { forecastText, levelText } from './text';

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

  it('trusts the charging flag of the hub over the 100 % rule (#421)', () => {
    const flagged = (hours: number[], percents: number[], charging: (boolean | undefined)[]) =>
      hours.map((h, i) => ({
        at: 1_000_000 + h * H,
        batPercent: percents[i],
        charging: charging[i],
      }));
    // A real 100 % at rest (flag false) is a value like any other.
    const rest = batteryForecast(flagged([0, 2, 4], [100, 98, 96], [false, false, false]));
    expect(rest?.drainPerHour).toBeCloseTo(1, 5);
    // Charging below 100 %: dropped, and the run starts after it.
    const f = batteryForecast(
      flagged(
        [0, 1, 2, 3, 4, 6],
        [80, 78, 90, 95, 70, 66],
        [undefined, undefined, true, true, undefined, undefined],
      ),
    );
    expect(f?.drainPerHour).toBeCloseTo(2, 5);
    expect(f?.windowS).toBe(2 * H);
    // Latest value flagged: charging, no forecast.
    const now = batteryForecast(flagged([0, 2, 3], [80, 78, 90], [undefined, undefined, true]));
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

/** Real values of the hub from 24.11. 10:00 (CET) on; 25.11. 06:12 to 19:53 it was off (#472). */
const OFF: [number, number][] = [
  [1795514336, 83],
  [1795520189, 85],
  [1795525539, 83],
  [1795529602, 85],
  [1795534998, 82],
  [1795540317, 81],
  [1795545718, 80],
  [1795551041, 79],
  [1795556700, 77],
  [1795562059, 75],
  [1795567423, 73],
  [1795572813, 71],
  [1795578179, 68],
  [1795583545, 66],
  [1795632818, 65],
  [1795636490, 63],
  [1795641768, 61],
  [1795647154, 59],
  [1795652480, 56],
  [1795657882, 53],
  [1795663210, 49],
  [1795668611, 45],
];
const off = (count: number) =>
  OFF.slice(0, count).map(([at, batPercent]) => ({ at, batPercent, charging: false }));

describe('battery forecast with gaps (#472)', () => {
  it('leaves the hours the hub was off out of the rate (real data)', () => {
    // 66 % at 06:12 before the gap up to 45 % at 05:50 the next day (OFF index 13 and 21).
    const forecasts = OFF.map((_, i) => batteryForecast(off(i + 1)));
    const emptyAt = (i: number) => forecasts[i]?.emptyAt ?? 0;
    // The gap used to pull the rate down to 0.84 %/h and move "empty" by three days
    // with the first value after it. Now the forecast moves by hours from value to value.
    for (let i = 13; i < OFF.length - 1; i += 1) {
      expect(Math.abs(emptyAt(i + 1) - emptyAt(i))).toBeLessThan(12 * H);
    }
    // Running time only: the 13.7 h gap is not part of the window.
    const last = forecasts[OFF.length - 1];
    expect(last?.windowS).toBeGreaterThan(22 * H);
    expect(last?.windowS).toBeLessThan(24 * H);
    expect(last?.drainPerHour).toBeGreaterThan(1.2);
    expect(last?.drainPerHour).toBeLessThan(2);
  });

  it('does not count a gap as running time or as drain', () => {
    // 2 %/h for 3 h, 10 h gap with 1 % lost, 2 %/h for 3 h more.
    const at = (h: number) => 1_000_000 + h * H;
    const samples = [
      { at: at(0), batPercent: 90 },
      { at: at(1), batPercent: 88 },
      { at: at(2), batPercent: 86 },
      { at: at(3), batPercent: 84 },
      { at: at(13), batPercent: 83 },
      { at: at(14), batPercent: 81 },
      { at: at(15), batPercent: 79 },
      { at: at(16), batPercent: 77 },
    ];
    const f = batteryForecast(samples);
    expect(f?.drainPerHour).toBeCloseTo(2, 5);
    expect(f?.windowS).toBe(6 * H);
    expect(f?.emptyAt).toBeCloseTo(at(16) + 38.5 * H, 0);
  });

  it('a gap of 3 h or less is still running time', () => {
    const f = batteryForecast(
      hourly([80, 78, 76, 74]).map((s, i) => ({ ...s, at: s.at + i * 2 * H })),
    );
    expect(f?.windowS).toBe(9 * H);
    expect(f?.drainPerHour).toBeCloseTo(2 / 3, 5);
  });

  it('takes two points of gauge noise after a full charge for no charge', () => {
    const f = batteryForecast(hourly([83, 85, 83, 85, 82, 81]));
    expect(f?.drainPerHour).toBeGreaterThan(0);
    expect(f?.emptyAt).toBeDefined();
    expect(f && forecastText(f, 1_000_000 + 5 * H)).not.toMatch(/Lädt/);
  });
});

describe('camera battery steps (#472)', () => {
  const at = (h: number) => 1_700_000_000 + h * H;
  const steps = (values: [number, number][]) =>
    values.map(([h, batPercent]) => ({ at: at(h), batPercent }));

  it('tells a camera that reports steps from one with percentages', () => {
    expect(
      reportsLevels(
        steps([
          [0, 80],
          [1, 20],
          [2, 80],
          [3, 100],
          [4, 0],
        ]),
      ),
    ).toBe(true);
    expect(
      reportsLevels(
        steps([
          [0, 80],
          [1, 77],
          [2, 74],
        ]),
      ),
    ).toBe(false);
    expect(reportsLevels([])).toBe(false);
  });

  it('shows the lowest value of 12 h, so toggling near a step does not flap', () => {
    // Real: 20 -> 80 -> 20 -> 80 within a day.
    const level = batteryLevel(
      steps([
        [0, 80],
        [1, 20],
        [4, 80],
        [7, 20],
        [9, 80],
      ]),
    );
    expect(level?.state).toBe('low');
    expect(level?.lowSince).toBe(at(1));
    expect(level && levelText(level, at(9))).toMatch(/^Akku schwach seit /);
    // 13 h later with no 20 % in between: good again.
    const later = batteryLevel(
      steps([
        [0, 80],
        [1, 20],
        [4, 80],
        [7, 20],
        [9, 80],
        [22, 80],
      ]),
    );
    expect(later?.state).toBe('good');
    expect(later?.lowSince).toBeUndefined();
    expect(later && levelText(later, at(22))).toBe('Akku gut.');
  });

  it('keeps the low phase over a toggle and ends it with a charge or after 24 h', () => {
    const toggle = batteryLevel(
      steps([
        [0, 20],
        [2, 80],
        [5, 20],
      ]),
    );
    expect(toggle?.lowSince).toBe(at(0));
    const charged = batteryLevel(
      steps([
        [0, 20],
        [2, 100],
        [5, 20],
      ]),
    );
    expect(charged?.lowSince).toBe(at(5));
    const late = batteryLevel(
      steps([
        [0, 20],
        [30, 20],
      ]),
    );
    expect(late?.lowSince).toBe(at(30));
  });

  it('knows empty, charging and a full charge', () => {
    expect(
      batteryLevel(
        steps([
          [0, 20],
          [1, 0],
        ]),
      )?.state,
    ).toBe('empty');
    expect(
      batteryLevel(
        steps([
          [0, 20],
          [1, 100],
        ]),
      )?.state,
    ).toBe('good');
    const flagged = batteryLevel([
      { at: at(0), batPercent: 20 },
      { at: at(1), batPercent: 100, charging: true },
    ]);
    expect(flagged?.state).toBe('charging');
    expect(flagged && levelText(flagged, at(1))).toBe('Lädt.');
    expect(batteryLevel([])).toBeUndefined();
  });

  it('mentions an old latest value', () => {
    const level = batteryLevel(steps([[0, 80]]));
    expect(level && levelText(level, at(10))).toMatch(/^Akku gut\. Letzter Wert von /);
  });
});
