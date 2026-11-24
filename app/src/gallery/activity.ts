import type { Recording } from './types';

/**
 * When do the cameras record (#443)? Pure functions over the recordings the
 * gallery already loads. `mediaTime` is the camera's wall clock read as if it
 * were UTC (docs/clock.md), so the hour and the weekday are read with the UTC
 * getters and no time zone is applied. The hub's read plan uses the same
 * local time.
 */

/** Recordings closer together than this are one event (a burst of the PIR sensor). */
export const BURST_S = 60;
/** A camera whose clock was never set reports a date before this. */
const FIRST_VALID_TIME = Date.UTC(2020, 0, 1) / 1000;
/** Minimum data before a plan is suggested. */
export const MIN_EVENTS = 50;
export const MIN_SPAN_DAYS = 14;
const DAY_S = 24 * 60 * 60;

export interface ActivityEvents {
  /** Event times, oldest first; bursts merged. */
  events: number[];
  /** Recordings left out because the camera's clock was not set. */
  ignored: number;
}

/** The events of a set of recordings: bursts count once, unset clocks are left out. */
export function activityEvents(
  recordings: readonly Pick<Recording, 'mediaTime'>[],
  /** Leave out recordings, e.g. empty triggers (#442). */
  exclude?: (r: Pick<Recording, 'mediaTime'>) => boolean,
): ActivityEvents {
  let ignored = 0;
  const times: number[] = [];
  for (const r of recordings) {
    if (!(r.mediaTime >= FIRST_VALID_TIME)) {
      ignored += 1;
    } else if (!exclude?.(r)) {
      times.push(r.mediaTime);
    }
  }
  times.sort((a, b) => a - b);
  const events: number[] = [];
  let last = -Infinity;
  for (const t of times) {
    if (t - last > BURST_S) events.push(t);
    last = t;
  }
  return { events, ignored };
}

export interface Heatmap {
  /** `cells[weekday][hour]`, weekday 0 = Monday. */
  cells: number[][];
  byHour: number[];
  total: number;
  max: number;
}

export function heatmap(events: readonly number[]): Heatmap {
  const cells = Array.from({ length: 7 }, () => new Array<number>(24).fill(0));
  const byHour = new Array<number>(24).fill(0);
  let max = 0;
  for (const t of events) {
    const d = new Date(t * 1000);
    const weekday = (d.getUTCDay() + 6) % 7;
    const hour = d.getUTCHours();
    const row = cells[weekday] as number[];
    row[hour] = (row[hour] ?? 0) + 1;
    byHour[hour] = (byHour[hour] ?? 0) + 1;
    max = Math.max(max, row[hour] ?? 0);
  }
  return { cells, byHour, total: events.length, max };
}

export interface DayCount {
  /** `YYYY-MM-DD` of the camera's clock. */
  day: string;
  count: number;
}

/** Events per day for the last `days` days up to the day of `until`, oldest first. */
export function eventsPerDay(events: readonly number[], until: number, days = 90): DayCount[] {
  const lastDay = Math.floor(until / DAY_S);
  const counts = new Array<number>(days).fill(0);
  for (const t of events) {
    const index = days - 1 - (lastDay - Math.floor(t / DAY_S));
    if (index >= 0 && index < days) counts[index] = (counts[index] ?? 0) + 1;
  }
  return counts.map((count, i) => ({
    day: new Date((lastDay - (days - 1 - i)) * DAY_S * 1000).toISOString().slice(0, 10),
    count,
  }));
}

const pad = (n: number) => String(n).padStart(2, '0');

/** A hour range as people say it: "16–19 Uhr" for hours 16, 17 and 18. */
function hoursText(start: number, end: number): string {
  return `${start}–${end === 0 ? 24 : end} Uhr`;
}

interface Run {
  start: number;
  /** Exclusive; may be above 24 for a run over midnight. */
  end: number;
  events: number;
}

/**
 * Runs of neighbouring hours, around the clock, where `keep(hour)` holds. A
 * gap of one hour does not break a run.
 */
function runs(byHour: readonly number[], keep: (hour: number) => boolean): Run[] {
  const on = byHour.map((_, h) => keep(h));
  if (on.every(Boolean) || !on.some(Boolean)) return [];
  const filled = on.map((v, h) => {
    const before = on[(h + 23) % 24] === true;
    const after = on[(h + 1) % 24] === true;
    return v || (before && after);
  });
  // Start at an hour that is off, so no run is cut at the array's edge.
  const origin = filled.findIndex((v) => !v);
  const result: Run[] = [];
  let current: Run | undefined;
  for (let i = 1; i <= 24; i++) {
    const h = (origin + i) % 24;
    if (filled[h]) {
      current ??= { start: origin + i, end: origin + i, events: 0 };
      current.end = origin + i + 1;
      current.events += byHour[h] ?? 0;
    } else if (current) {
      result.push(current);
      current = undefined;
    }
  }
  if (current) result.push(current);
  return result.map((r) => ({
    ...r,
    start: r.start % 24,
    end: r.end - (r.start - (r.start % 24)),
  }));
}

/**
 * A sentence on the busiest and the quietest part of the day, such as
 * "Meiste Aktivität 16–19 Uhr (42 %), ruhig 10–14 Uhr"; empty without data.
 */
export function summaryText(byHour: readonly number[]): string {
  const total = byHour.reduce((a, b) => a + b, 0);
  if (total === 0) return '';
  const busy = runs(byHour, (h) => (byHour[h] ?? 0) >= (total / 24) * 1.5);
  const top = [...busy].sort((a, b) => b.events - a.events)[0];
  const quiet = runs(byHour, (h) => (byHour[h] ?? 0) <= (total / 24) * 0.25);
  const calm = [...quiet].sort((a, b) => b.end - b.start - (a.end - a.start))[0];
  const parts: string[] = [];
  if (top) {
    const share = Math.round((top.events / total) * 100);
    const hours = hoursText(top.start, top.end % 24);
    parts.push(`Meiste Aktivität ${hours} (${share} %)`);
  }
  if (calm && calm.end - calm.start >= 3) {
    parts.push(`ruhig ${hoursText(calm.start, calm.end % 24)}`);
  }
  return parts.join(', ');
}

export type PlanSuggestion =
  { ok: true; plan: string; windows: string[]; coverage: number } | { ok: false; reason: string };

/** Interval inside a suggested window and for the rest of the day, minutes. */
const WINDOW_MIN = 30;
const REST_MIN = 120;
const MAX_WINDOWS = 3;

/**
 * A camera read plan from the hours with the most events: windows of 30
 * minutes there, 2 hours elsewhere. Fixed times only, because the backend's
 * plan editor does not take sun-relative bounds yet (docs/power-and-schedule.md).
 */
export function suggestPlan(events: readonly number[]): PlanSuggestion {
  const first = events[0];
  const last = events[events.length - 1];
  if (events.length < MIN_EVENTS || first === undefined || last === undefined) {
    const reason = `Noch zu wenig Aufnahmen (mindestens ${MIN_EVENTS} nötig).`;
    return { ok: false, reason };
  }
  if (last - first < MIN_SPAN_DAYS * DAY_S) {
    const reason = `Die Aufnahmen decken noch keine ${MIN_SPAN_DAYS} Tage ab.`;
    return { ok: false, reason };
  }
  const { byHour, total } = heatmap(events);
  const busy = runs(byHour, (h) => (byHour[h] ?? 0) >= (total / 24) * 1.5);
  const biggest = busy.sort((a, b) => b.events - a.events).slice(0, MAX_WINDOWS);
  const found = biggest.sort((a, b) => a.start - b.start);
  if (found.length === 0) {
    return {
      ok: false,
      reason: 'Die Aufnahmen verteilen sich gleichmäßig; ein Plan bringt nichts.',
    };
  }
  const clock = (h: number) => `${pad(h % 24 === 0 && h > 0 ? 24 : h % 24)}:00`;
  const windows = found.map((r) => `${clock(r.start)}-${clock(r.end)}`);
  const covered = found.reduce((sum, r) => sum + r.events, 0);
  const entries = windows.map((w) => `${w}=${WINDOW_MIN}`);
  entries.push(`*=${REST_MIN}`);
  return { ok: true, plan: entries.join(','), windows, coverage: covered / total };
}
