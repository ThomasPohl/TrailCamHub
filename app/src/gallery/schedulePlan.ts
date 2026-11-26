/**
 * Camera read plans for the schedule editor (#431, #436): parsed into windows
 * the editor can show and change, and written back in the form the firmware
 * prints (components/schedule/schedule_plan.cpp), so the text field and the
 * editor stay in sync.
 *
 *   "sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120"
 *
 * A window bound is a clock time or the sun with an offset in minutes.
 */

export type Bound =
  | { kind: 'clock'; minute: number }
  | { kind: 'sunrise' | 'sunset'; offset: number };

export interface PlanWindow {
  start: Bound;
  end: Bound;
  /** Minutes between reads inside the window; 0: none. */
  intervalMin: number;
}

export interface EditPlan {
  windows: PlanWindow[];
  /** Minutes between reads outside the windows; 0: none. */
  defaultMin: number;
}

export type PlanParse = { ok: true; plan: EditPlan } | { ok: false; error: string };

export const MAX_WINDOWS = 8;
const MAX_INTERVAL_MIN = 1440;
const MAX_SUN_OFFSET = 720;

function parseClock(text: string): number | undefined {
  const match = /^(\d\d):(\d\d)$/.exec(text);
  if (!match) return undefined;
  const h = Number(match[1]);
  const m = Number(match[2]);
  if (m > 59 || h > 24 || (h === 24 && m !== 0)) return undefined;
  return h * 60 + m;
}

function parseBound(text: string): Bound | undefined {
  const sun = /^(sunrise|sunset)(?:([+-])(\d{1,3}))?$/.exec(text);
  if (sun) {
    const value = sun[3] === undefined ? 0 : Number(sun[3]);
    if (value > MAX_SUN_OFFSET) return undefined;
    return { kind: sun[1] as 'sunrise' | 'sunset', offset: sun[2] === '-' ? -value : value };
  }
  const minute = parseClock(text);
  return minute === undefined ? undefined : { kind: 'clock', minute };
}

function sameBound(a: Bound, b: Bound): boolean {
  if (a.kind === 'clock' || b.kind === 'clock') {
    return a.kind === 'clock' && b.kind === 'clock' && a.minute === b.minute;
  }
  return a.kind === b.kind && a.offset === b.offset;
}

/** Reads a plan like the firmware; "off" or "" is a plan without reads. */
export function parseEditPlan(text: string): PlanParse {
  const plan: EditPlan = { windows: [], defaultMin: 0 };
  let haveDefault = false;
  for (const entry of text.split(/[,; \t]+/).filter((e) => e !== '')) {
    if (entry === 'off') continue;
    const equals = entry.indexOf('=');
    if (equals < 0) return { ok: false, error: `„${entry}“: es fehlt „=Minuten“` };
    const range = entry.slice(0, equals);
    const minutesText = entry.slice(equals + 1);
    const minutes = /^\d{1,5}$/.test(minutesText) ? Number(minutesText) : NaN;
    if (!(minutes >= 0 && minutes <= MAX_INTERVAL_MIN)) {
      return { ok: false, error: `„${entry}“: Abstand 0 bis 1440 Minuten` };
    }
    if (range === '*') {
      if (haveDefault) return { ok: false, error: '„*“ steht zweimal da' };
      haveDefault = true;
      plan.defaultMin = minutes;
      continue;
    }
    let start: Bound | undefined;
    let end: Bound | undefined;
    const dots = range.indexOf('..');
    if (dots >= 0) {
      start = parseBound(range.slice(0, dots));
      end = parseBound(range.slice(dots + 2));
    } else {
      const dash = range.indexOf('-');
      const s = dash < 0 ? undefined : parseClock(range.slice(0, dash));
      const e = dash < 0 ? undefined : parseClock(range.slice(dash + 1));
      if (s !== undefined) start = { kind: 'clock', minute: s };
      if (e !== undefined) end = { kind: 'clock', minute: e };
    }
    if (!start || !end || (start.kind === 'clock' && start.minute === 1440)) {
      return {
        ok: false,
        error: `„${range}“: Zeitfenster wie 05:00-09:00 oder sunrise-60..sunrise+90`,
      };
    }
    if (sameBound(start, end)) return { ok: false, error: `„${range}“ ist leer` };
    if (plan.windows.length >= MAX_WINDOWS) return { ok: false, error: 'Höchstens 8 Zeitfenster' };
    plan.windows.push({ start, end, intervalMin: minutes });
  }
  return { ok: true, plan };
}

function clockText(minute: number): string {
  return `${String(Math.floor(minute / 60)).padStart(2, '0')}:${String(minute % 60).padStart(2, '0')}`;
}

function boundText(bound: Bound): string {
  if (bound.kind === 'clock') return clockText(bound.minute);
  if (bound.offset === 0) return bound.kind;
  return `${bound.kind}${bound.offset < 0 ? '-' : '+'}${Math.abs(bound.offset)}`;
}

export function planReads(plan: EditPlan): boolean {
  return plan.defaultMin > 0 || plan.windows.some((w) => w.intervalMin > 0);
}

/** The plan in the firmware's normalized form ("off" for no reads). */
export function formatEditPlan(plan: EditPlan): string {
  if (!planReads(plan)) return 'off';
  const parts = plan.windows.map((w) =>
    w.start.kind === 'clock' && w.end.kind === 'clock'
      ? `${clockText(w.start.minute)}-${clockText(w.end.minute)}=${w.intervalMin}`
      : `${boundText(w.start)}..${boundText(w.end)}=${w.intervalMin}`,
  );
  parts.push(`*=${plan.defaultMin}`);
  return parts.join(',');
}

/** Sunrise and sunset as local minutes of the day. */
export interface SunTimes {
  sunrise: number;
  sunset: number;
}

/** Used for the bar when the hub has not reported sun times yet: an autumn day in Germany. */
export const DEFAULT_SUN: SunTimes = { sunrise: 7 * 60 + 30, sunset: 17 * 60 };

function resolve(bound: Bound, sun: SunTimes): number {
  if (bound.kind === 'clock') return bound.minute;
  const base = bound.kind === 'sunrise' ? sun.sunrise : sun.sunset;
  return (((base + bound.offset) % 1440) + 1440) % 1440;
}

function contains(start: number, end: number, minute: number): boolean {
  return start < end ? minute >= start && minute < end : minute >= start || minute < end;
}

/** Interval in minutes at a local minute of the day; the first window that contains it applies. */
export function intervalAt(plan: EditPlan, minute: number, sun: SunTimes): number {
  for (const w of plan.windows) {
    const s = resolve(w.start, sun);
    const e = resolve(w.end, sun);
    if (s !== e && contains(s, e, minute)) return w.intervalMin;
  }
  return plan.defaultMin;
}

export interface Segment {
  /** Local minutes of the day, `end` exclusive. */
  start: number;
  end: number;
  intervalMin: number;
  /** Inside a window (not the "rest of the day" interval). */
  window: boolean;
}

/** The day from 0:00 to 24:00 cut into runs of the same interval, for the 24-hour bar. */
export function daySegments(plan: EditPlan, sun: SunTimes): Segment[] {
  const inWindow = (minute: number) =>
    plan.windows.some((w) => {
      const s = resolve(w.start, sun);
      const e = resolve(w.end, sun);
      return s !== e && contains(s, e, minute);
    });
  const segments: Segment[] = [];
  for (let minute = 0; minute < 1440; minute++) {
    const intervalMin = intervalAt(plan, minute, sun);
    const window = inWindow(minute);
    const last = segments[segments.length - 1];
    if (last && last.intervalMin === intervalMin && last.window === window) last.end = minute + 1;
    else segments.push({ start: minute, end: minute + 1, intervalMin, window });
  }
  return segments;
}

/** Reads per day at that plan (without quiet hours), for a hint next to the editor. */
export function readsPerDay(plan: EditPlan, sun: SunTimes): number {
  return daySegments(plan, sun).reduce(
    (sum, s) => sum + (s.intervalMin > 0 ? (s.end - s.start) / s.intervalMin : 0),
    0,
  );
}

export interface Preset {
  name: string;
  hint: string;
  plan: string;
}

export const PRESETS: Preset[] = [
  {
    name: 'Dämmerung (Standard)',
    hint: 'Morgens und abends alle 30 Minuten, sonst alle 2 Stunden.',
    plan: 'sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120',
  },
  {
    name: 'Ganztägig',
    hint: 'Den ganzen Tag stündlich.',
    plan: '*=60',
  },
  {
    name: 'Sparsam',
    hint: 'Morgens und abends stündlich, sonst alle 4 Stunden.',
    plan: 'sunrise-30..sunrise+60=60,sunset-60..sunset+30=60,*=240',
  },
];

/** Contact intervals offered to the user (minutes); the backend takes 5-1440. */
export const CONTACT_STEPS = [15, 30, 60, 120, 240, 480, 720, 1440];

/** "alle 30 Minuten", "alle 2 Stunden", "einmal am Tag". */
export function everyText(minutes: number): string {
  if (minutes <= 0) return 'nie';
  if (minutes === 1440) return 'einmal am Tag';
  if (minutes % 60 === 0) {
    const h = minutes / 60;
    return h === 1 ? 'jede Stunde' : `alle ${h} Stunden`;
  }
  return `alle ${minutes} Minuten`;
}

/** A bound in words: "06:30", "Sonnenaufgang − 60 min", "Sonnenuntergang". */
export function boundWords(bound: Bound): string {
  if (bound.kind === 'clock') return clockText(bound.minute);
  const name = bound.kind === 'sunrise' ? 'Sonnenaufgang' : 'Sonnenuntergang';
  if (bound.offset === 0) return name;
  return `${name} ${bound.offset < 0 ? '−' : '+'} ${Math.abs(bound.offset)} min`;
}

export { clockText };

/** Quiet hours "23:00-06:00" as minutes; undefined for "off", "" or nonsense. */
export function parseQuietText(text: string): { start: number; end: number } | undefined {
  const match = /^(\d\d:\d\d)-(\d\d:\d\d)$/.exec(text.trim());
  if (!match) return undefined;
  const start = parseClock(match[1] ?? '');
  const end = parseClock(match[2] ?? '');
  if (start === undefined || end === undefined || start === 1440 || start === end) return undefined;
  return { start, end };
}

/** "52.52, 13.405" → "52.5,13.4"; undefined if it is no location. */
export function normalizeLocation(text: string): string | undefined {
  const match = /^\s*([+-]?\d{1,3}(?:\.\d+)?)\s*[,; ]\s*([+-]?\d{1,3}(?:\.\d+)?)\s*$/.exec(text);
  if (!match) return undefined;
  const lat = Number(match[1]);
  const lon = Number(match[2]);
  if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return undefined;
  const round = (v: number) => (Math.round(v * 10) / 10 + 0).toFixed(1);
  return `${round(lat)},${round(lon)}`;
}
