import { describe, expect, it } from 'vitest';
import {
  activityEvents,
  BURST_S,
  eventsPerDay,
  heatmap,
  MIN_EVENTS,
  suggestPlan,
  summaryText,
} from './activity';

/** Camera wall clock for a date and time (as if UTC). */
const at = (day: string, hhmm: string) => Date.parse(`${day}T${hhmm}:00Z`) / 1000;
const rec = (mediaTime: number) => ({ mediaTime });

describe('activityEvents', () => {
  it('merges bursts, sorts and leaves out cameras whose clock was never set', () => {
    const t = at('2026-11-10', '17:00');
    const { events, ignored } = activityEvents([
      rec(t + BURST_S),
      rec(t),
      rec(t + 2 * BURST_S + 1),
      rec(946_684_800), // 2000-01-01: clock not set
      rec(0),
    ]);
    expect(events).toEqual([t, t + 2 * BURST_S + 1]);
    expect(ignored).toBe(2);
  });

  it('chains a long burst into one event', () => {
    const t = at('2026-11-10', '17:00');
    const { events } = activityEvents([0, 50, 100, 150].map((s) => rec(t + s)));
    expect(events).toEqual([t]);
  });

  it('can leave out recordings, e.g. empty triggers', () => {
    const t = at('2026-11-10', '17:00');
    const { events } = activityEvents([rec(t), rec(t + 600)], (r) => r.mediaTime === t);
    expect(events).toEqual([t + 600]);
  });
});

describe('heatmap', () => {
  it('counts weekday (Monday first) and hour of the camera clock', () => {
    // 2026-11-23 is a Monday, 2026-11-29 a Sunday; midnight and 23:59 stay in their day.
    const map = heatmap([
      at('2026-11-23', '00:00'),
      at('2026-11-23', '00:30'),
      at('2026-11-29', '23:59'),
    ]);
    expect(map.cells[0]?.[0]).toBe(2);
    expect(map.cells[6]?.[23]).toBe(1);
    expect(map.byHour[0]).toBe(2);
    expect(map.total).toBe(3);
    expect(map.max).toBe(2);
  });
});

describe('eventsPerDay', () => {
  it('gives one entry per day up to the last one, oldest first', () => {
    const until = at('2026-11-24', '12:00');
    const days = eventsPerDay([at('2026-11-24', '01:00'), at('2026-11-22', '23:59')], until, 4);
    expect(days).toEqual([
      { day: '2026-11-21', count: 0 },
      { day: '2026-11-22', count: 1 },
      { day: '2026-11-23', count: 0 },
      { day: '2026-11-24', count: 1 },
    ]);
  });

  it('ignores events outside the window', () => {
    const until = at('2026-11-24', '12:00');
    expect(eventsPerDay([at('2026-01-01', '12:00')], until, 3).every((d) => d.count === 0)).toBe(
      true,
    );
  });
});

/** `perDay` events in each of the given hours on `days` consecutive days (10 min apart). */
function pattern(hours: number[], days: number, perDay = 3) {
  const out: number[] = [];
  for (let d = 0; d < days; d++) {
    const day = new Date(Date.UTC(2026, 9, 1 + d)).toISOString().slice(0, 10);
    for (const h of hours) {
      for (let i = 0; i < perDay; i++) out.push(at(day, `${pad(h)}:${pad(i * 10)}`));
    }
  }
  return out.sort((a, b) => a - b);
}
const pad = (n: number) => String(n).padStart(2, '0');

describe('suggestPlan', () => {
  it('needs enough events and days', () => {
    expect(suggestPlan(pattern([17], 3))).toMatchObject({ ok: false });
    expect(suggestPlan(pattern([17], 10, 6)).ok).toBe(false);
    expect(MIN_EVENTS).toBe(50);
  });

  it('suggests one window for one peak', () => {
    const result = suggestPlan(pattern([16, 17, 18], 20));
    expect(result).toMatchObject({ ok: true, plan: '16:00-19:00=30,*=120', coverage: 1 });
  });

  it('suggests two windows for a morning and an evening peak, in clock order', () => {
    const result = suggestPlan(pattern([5, 6, 7, 18, 19, 20], 20));
    expect(result).toMatchObject({ ok: true, plan: '05:00-08:00=30,18:00-21:00=30,*=120' });
  });

  it('bridges a one-hour gap and ends at 24:00', () => {
    const result = suggestPlan(pattern([21, 22, 23], 20));
    expect(result).toMatchObject({ ok: true, plan: '21:00-24:00=30,*=120' });
    const gap = suggestPlan(pattern([5, 7], 20));
    expect(gap).toMatchObject({ ok: true, plan: '05:00-08:00=30,*=120' });
  });

  it('follows a peak over midnight', () => {
    const result = suggestPlan(pattern([22, 23, 0, 1], 20));
    expect(result).toMatchObject({ ok: true, plan: '22:00-02:00=30,*=120' });
  });

  it('suggests nothing for a flat distribution', () => {
    const all = Array.from({ length: 24 }, (_, h) => h);
    expect(suggestPlan(pattern(all, 20, 3))).toMatchObject({ ok: false });
  });

  it('keeps the three busiest windows', () => {
    const result = suggestPlan(pattern([1, 5, 9, 13, 17], 20));
    expect(result.ok && result.windows).toHaveLength(3);
  });
});

describe('summaryText', () => {
  it('names the busiest and the quietest part of the day', () => {
    const byHour = new Array<number>(24).fill(10);
    for (const h of [16, 17, 18]) byHour[h] = 40;
    for (const h of [10, 11, 12, 13]) byHour[h] = 0;
    expect(summaryText(byHour)).toBe('Meiste Aktivität 16–19 Uhr (41 %), ruhig 10–14 Uhr');
  });

  it('is empty without events', () => {
    expect(summaryText(new Array<number>(24).fill(0))).toBe('');
  });
});
