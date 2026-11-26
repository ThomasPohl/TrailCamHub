import { describe, expect, it } from 'vitest';
import {
  daySegments,
  DEFAULT_SUN,
  everyText,
  formatEditPlan,
  intervalAt,
  normalizeLocation,
  parseEditPlan,
  parseQuietText,
  PRESETS,
  readsPerDay,
} from './schedulePlan';

function normalized(text: string): string {
  const result = parseEditPlan(text);
  if (!result.ok) throw new Error(result.error);
  return formatEditPlan(result.plan);
}

describe('read plans in the editor (#431, #436)', () => {
  it('reads and writes plans in the firmware form', () => {
    expect(normalized('05:00-09:00=30, 17:00-22:00=30 *=120')).toBe(
      '05:00-09:00=30,17:00-22:00=30,*=120',
    );
    expect(normalized('sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120')).toBe(
      'sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120',
    );
    expect(normalized('sunrise+0..09:00=15')).toBe('sunrise..09:00=15,*=0');
    expect(normalized('05:00..07:00=10,*=60')).toBe('05:00-07:00=10,*=60');
    expect(normalized('off')).toBe('off');
    expect(normalized('')).toBe('off');
    expect(normalized('*=0')).toBe('off');
  });

  it('rejects what the hub would reject, with a German reason', () => {
    for (const text of [
      '*',
      '*=1441',
      '05:00=30',
      '05:00-05:00=30',
      'sunrise..sunrise=30',
      'noon..sunset=30',
      '*=10,*=20',
    ]) {
      const result = parseEditPlan(text);
      expect(result.ok, text).toBe(false);
      if (!result.ok) expect(result.error).not.toBe('');
    }
  });

  it('every preset is a valid plan', () => {
    for (const preset of PRESETS) {
      expect(normalized(preset.plan), preset.name).toBe(preset.plan);
    }
  });

  it('resolves sun windows on a day and cuts the day into segments', () => {
    const result = parseEditPlan('sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120');
    if (!result.ok) throw new Error(result.error);
    const sun = { sunrise: 8 * 60, sunset: 16 * 60 };
    expect(intervalAt(result.plan, 7 * 60, sun)).toBe(30);
    expect(intervalAt(result.plan, 12 * 60, sun)).toBe(120);
    expect(intervalAt(result.plan, 14 * 60 + 30, sun)).toBe(30);
    const segments = daySegments(result.plan, sun);
    expect(segments.map((s) => [s.start, s.end, s.intervalMin, s.window])).toEqual([
      [0, 420, 120, false],
      [420, 570, 30, true],
      [570, 870, 120, false],
      [870, 1020, 30, true],
      [1020, 1440, 120, false],
    ]);
    // 2 × 2.5 h at 30 min = 10, 19 h at 120 min = 9.5.
    expect(readsPerDay(result.plan, sun)).toBeCloseTo(19.5, 5);
  });

  it('a window over midnight wraps', () => {
    const result = parseEditPlan('22:00-02:00=15,*=0');
    if (!result.ok) throw new Error(result.error);
    expect(intervalAt(result.plan, 23 * 60, DEFAULT_SUN)).toBe(15);
    expect(intervalAt(result.plan, 60, DEFAULT_SUN)).toBe(15);
    expect(intervalAt(result.plan, 3 * 60, DEFAULT_SUN)).toBe(0);
  });

  it('words intervals', () => {
    expect(everyText(30)).toBe('alle 30 Minuten');
    expect(everyText(60)).toBe('jede Stunde');
    expect(everyText(120)).toBe('alle 2 Stunden');
    expect(everyText(1440)).toBe('einmal am Tag');
    expect(everyText(0)).toBe('nie');
  });

  it('reads quiet hours and locations', () => {
    expect(parseQuietText('23:00-06:00')).toEqual({ start: 1380, end: 360 });
    expect(parseQuietText('off')).toBeUndefined();
    expect(parseQuietText('23:00-23:00')).toBeUndefined();
    expect(normalizeLocation('52.52, 13.405')).toBe('52.5,13.4');
    expect(normalizeLocation('-0.04;0')).toBe('0.0,0.0');
    expect(normalizeLocation('95,10')).toBeUndefined();
    expect(normalizeLocation('Berlin')).toBeUndefined();
  });
});
