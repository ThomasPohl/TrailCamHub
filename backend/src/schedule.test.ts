import { describe, expect, it } from 'vitest';
import {
  formatLocation,
  formatPlan,
  formatQuiet,
  parseLocation,
  parsePlan,
  parseQuiet,
  planUsesSun,
} from './schedule';

function normalized(text: string): string {
  const result = parsePlan(text);
  if (!result.ok) throw new Error(result.error);
  return formatPlan(result.plan);
}

describe('camera read plans', () => {
  it('parses and normalizes plans like the firmware', () => {
    expect(normalized('05:00-09:00=30, 17:00-22:00=30 *=120')).toBe(
      '05:00-09:00=30,17:00-22:00=30,*=120',
    );
    expect(normalized('*=120')).toBe('*=120');
    expect(normalized('20:00-24:00=15')).toBe('20:00-24:00=15,*=0');
    expect(normalized('22:00-06:00=0,*=60')).toBe('22:00-06:00=0,*=60');
    expect(normalized('off')).toBe('off');
    expect(normalized('')).toBe('off');
    expect(normalized('*=0')).toBe('off');
  });

  it('rejects invalid plans with a reason', () => {
    for (const text of [
      '*',
      '*=abc',
      '*=1441',
      '*=-5',
      '5:00-9:00=30',
      '05:00=30',
      '05:00-05:00=30',
      '25:00-26:00=10',
      '05:60-06:00=10',
      '24:00-01:00=10',
      '*=10,*=20',
      Array.from({ length: 9 }, (_, i) => `0${i}:00-0${i}:30=1`).join(','),
    ]) {
      const result = parsePlan(text);
      expect(result.ok, text).toBe(false);
      if (!result.ok) expect(result.error).not.toBe('');
    }
  });
});

describe('windows at the sun (#436)', () => {
  it('parses and normalizes sun bounds like the firmware', () => {
    expect(normalized('sunrise-60..sunrise+90=30, sunset-90..sunset+60=30,*=120')).toBe(
      'sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120',
    );
    expect(normalized('sunrise+0..09:00=15')).toBe('sunrise..09:00=15,*=0');
    expect(normalized('05:00..sunrise=10,*=60')).toBe('05:00..sunrise=10,*=60');
    // Two clock bounds with '..' are the plain form.
    expect(normalized('05:00..07:00=10,*=60')).toBe('05:00-07:00=10,*=60');
  });

  it('tells whether a plan uses the sun', () => {
    const sun = parsePlan('sunset-30..sunset+30=10,*=120');
    const clock = parsePlan('05:00-09:00=30,*=120');
    expect(sun.ok && planUsesSun(sun.plan)).toBe(true);
    expect(clock.ok && planUsesSun(clock.plan)).toBe(false);
  });

  it('rejects invalid sun bounds', () => {
    for (const text of [
      'sunrise..sunrise=30',
      'sunrise-10..sunrise-10=30',
      'sunrise+721..sunset=30',
      'sunrise+..sunset=30',
      'noon..sunset=30',
      'sunrise..25:00=30',
      '24:00..sunset=30',
    ]) {
      expect(parsePlan(text).ok, text).toBe(false);
    }
  });
});

describe('quiet hours and location (#436)', () => {
  it('parses quiet hours like the firmware', () => {
    const q = (text: string) => {
      const result = parseQuiet(text);
      if (!result.ok) throw new Error(result.error);
      return formatQuiet(result.quiet);
    };
    expect(q('23:00-06:00')).toBe('23:00-06:00');
    expect(q(' 22:30-05:15 ')).toBe('22:30-05:15');
    expect(q('off')).toBe('off');
    expect(q('')).toBe('off');
    for (const bad of ['23:00', '23:00-23:00', '24:00-06:00', '23-06', '23:00-6:00']) {
      expect(parseQuiet(bad).ok, bad).toBe(false);
    }
  });

  it('parses a location and rounds it to 0.1 degrees', () => {
    const l = (text: string) => {
      const result = parseLocation(text);
      if (!result.ok) throw new Error(result.error);
      return formatLocation(result.location);
    };
    expect(l('52.52,13.405')).toBe('52.5,13.4');
    expect(l('48.14; 11.58')).toBe('48.1,11.6');
    expect(l('-33.87, 151.21')).toBe('-33.9,151.2');
    expect(l('-0.04,0')).toBe('0.0,0.0');
    for (const bad of ['52.5', '91,10', '10,181', 'abc,def', '52.5,13.4,1']) {
      expect(parseLocation(bad).ok, bad).toBe(false);
    }
  });
});
