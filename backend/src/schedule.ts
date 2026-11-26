/**
 * Camera read plans: time windows in the hub's local time, each with an
 * interval in minutes, and `*` for the rest of the day, e.g.
 * `05:00-09:00=30,17:00-22:00=30,*=120`. A window may also start or end at
 * the sun: `sunrise-60..sunrise+90=30` (#436); `..` separates such bounds. Mirrors the firmware's parser
 * (components/schedule/schedule_plan.cpp) so a plan set in the app is
 * checked before it reaches the hub, and stored in the same normalized form
 * the hub reports back.
 */

export const MAX_WINDOWS = 8;
export const MAX_INTERVAL_MIN = 1440;
export const MAX_PLAN_LENGTH = 200;
/** Check-in intervals that can be set from the app (seconds). */
export const MIN_CHECKIN_INTERVAL_S = 5 * 60;
export const MAX_CHECKIN_INTERVAL_S = 24 * 60 * 60;

/** A window bound at the sun, with an offset in minutes (at most ±720). */
export interface SunBound {
  sun: 'sunrise' | 'sunset';
  offsetMin: number;
}

export interface PlanWindow {
  /**
   * Minutes of the day; `end` is exclusive, may be 1440 and may be < start.
   * Ignored for a bound that is at the sun.
   */
  start: number;
  end: number;
  intervalMin: number;
  /** Bounds at the sun (#436); absent: the clock bound applies. */
  startSun?: SunBound;
  endSun?: SunBound;
}

export interface Plan {
  windows: PlanWindow[];
  defaultMin: number;
}

export type PlanResult = { ok: true; plan: Plan } | { ok: false; error: string };

function parseClock(text: string): number | undefined {
  const match = /^(\d\d):(\d\d)$/.exec(text);
  if (!match) return undefined;
  const hours = Number(match[1]);
  const minutes = Number(match[2]);
  if (minutes > 59 || hours > 24 || (hours === 24 && minutes !== 0)) return undefined;
  return hours * 60 + minutes;
}

const MAX_SUN_OFFSET_MIN = 720;

function parseSunBound(text: string): SunBound | undefined {
  const match = /^(sunrise|sunset)(?:([+-])(\d{1,3}))?$/.exec(text);
  if (!match) return undefined;
  const value = match[3] === undefined ? 0 : Number(match[3]);
  if (value > MAX_SUN_OFFSET_MIN) return undefined;
  return { sun: match[1] as SunBound['sun'], offsetMin: match[2] === '-' ? -value : value };
}

function sameSun(a: SunBound | undefined, b: SunBound | undefined): boolean {
  return a !== undefined && b !== undefined && a.sun === b.sun && a.offsetMin === b.offsetMin;
}

export function parsePlan(text: string): PlanResult {
  const plan: Plan = { windows: [], defaultMin: 0 };
  let haveDefault = false;
  for (const entry of text.split(/[,; \t]+/).filter((e) => e !== '')) {
    if (entry === 'off') continue;
    const equals = entry.indexOf('=');
    if (equals < 0) return { ok: false, error: `entry '${entry}' has no '=MIN'` };
    const range = entry.slice(0, equals);
    const minutesText = entry.slice(equals + 1);
    const minutes = /^\d{1,5}$/.test(minutesText) ? Number(minutesText) : NaN;
    if (!(minutes >= 0 && minutes <= MAX_INTERVAL_MIN)) {
      return { ok: false, error: `interval in '${entry}' must be 0-1440 minutes` };
    }
    if (range === '*') {
      if (haveDefault) return { ok: false, error: "'*' is given twice" };
      haveDefault = true;
      plan.defaultMin = minutes;
      continue;
    }
    const dots = range.indexOf('..');
    if (dots >= 0) {
      const first = range.slice(0, dots);
      const second = range.slice(dots + 2);
      const startSun = parseSunBound(first);
      const endSun = parseSunBound(second);
      const start = startSun ? 0 : parseClock(first);
      const end = endSun ? 0 : parseClock(second);
      if (start === undefined || end === undefined || (!startSun && start === 1440)) {
        return {
          ok: false,
          error: `window '${range}' needs HH:MM, sunrise or sunset (with +-minutes) on both sides`,
        };
      }
      if ((!startSun && !endSun && start === end) || sameSun(startSun, endSun)) {
        return { ok: false, error: `window '${range}' is empty` };
      }
      if (plan.windows.length >= MAX_WINDOWS) return { ok: false, error: 'at most 8 windows' };
      const window: PlanWindow = { start, end, intervalMin: minutes };
      if (startSun) window.startSun = startSun;
      if (endSun) window.endSun = endSun;
      plan.windows.push(window);
      continue;
    }
    const dash = range.indexOf('-');
    const start = dash < 0 ? undefined : parseClock(range.slice(0, dash));
    const end = dash < 0 ? undefined : parseClock(range.slice(dash + 1));
    if (start === undefined || end === undefined || start === 1440) {
      return { ok: false, error: `window '${range}' must look like HH:MM-HH:MM` };
    }
    if (start === end) return { ok: false, error: `window '${range}' is empty` };
    if (plan.windows.length >= MAX_WINDOWS) return { ok: false, error: 'at most 8 windows' };
    plan.windows.push({ start, end, intervalMin: minutes });
  }
  return { ok: true, plan };
}

function clockText(minutes: number): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}`;
}

export function planReads(plan: Plan): boolean {
  return plan.defaultMin > 0 || plan.windows.some((w) => w.intervalMin > 0);
}

/** The plan in the form the firmware prints ("off" for no reads). */
function boundText(clock: number, sun: SunBound | undefined): string {
  if (!sun) return clockText(clock);
  if (sun.offsetMin === 0) return sun.sun;
  return `${sun.sun}${sun.offsetMin < 0 ? '-' : '+'}${Math.abs(sun.offsetMin)}`;
}

/** True if a window of the plan starts or ends at the sun. */
export function planUsesSun(plan: Plan): boolean {
  return plan.windows.some((w) => w.startSun !== undefined || w.endSun !== undefined);
}

export function formatPlan(plan: Plan): string {
  if (!planReads(plan)) return 'off';
  const parts = plan.windows.map((w) =>
    w.startSun || w.endSun
      ? `${boundText(w.start, w.startSun)}..${boundText(w.end, w.endSun)}=${w.intervalMin}`
      : `${clockText(w.start)}-${clockText(w.end)}=${w.intervalMin}`,
  );
  parts.push(`*=${plan.defaultMin}`);
  return parts.join(',');
}

/** Quiet hours (#436): one window in local time, or none. */
export interface Quiet {
  start: number;
  end: number;
}

export type QuietResult = { ok: true; quiet: Quiet | undefined } | { ok: false; error: string };

/** Parses "HH:MM-HH:MM" (may span midnight); "off" means none. Like the firmware. */
export function parseQuiet(text: string): QuietResult {
  const trimmed = text.replace(/[,; \t]+/g, '');
  if (trimmed === '' || trimmed === 'off') return { ok: true, quiet: undefined };
  const dash = trimmed.indexOf('-');
  const start = dash < 0 ? undefined : parseClock(trimmed.slice(0, dash));
  const end = dash < 0 ? undefined : parseClock(trimmed.slice(dash + 1));
  if (start === undefined || end === undefined || start === 1440) {
    return { ok: false, error: `quiet hours '${trimmed}' must look like HH:MM-HH:MM` };
  }
  if (start === end) return { ok: false, error: `quiet hours '${trimmed}' are empty` };
  return { ok: true, quiet: { start, end } };
}

/** The quiet hours as the firmware prints them ("off" for none). */
export function formatQuiet(quiet: Quiet | undefined): string {
  return quiet ? `${clockText(quiet.start)}-${clockText(quiet.end)}` : 'off';
}

export interface Location {
  latitude: number;
  longitude: number;
}

export type LocationResult = { ok: true; location: Location } | { ok: false; error: string };

/** Parses "LAT,LON" in decimal degrees and rounds to 0.1 degrees (about 10 km), like the firmware. */
export function parseLocation(text: string): LocationResult {
  const match = /^\s*([+-]?\d{1,3}(?:\.\d+)?)\s*[,; ]\s*([+-]?\d{1,3}(?:\.\d+)?)\s*$/.exec(text);
  if (!match) return { ok: false, error: 'location must look like 52.5,13.4' };
  const latitude = Number(match[1]);
  const longitude = Number(match[2]);
  if (latitude < -90 || latitude > 90) {
    return { ok: false, error: 'latitude must be between -90 and 90' };
  }
  if (longitude < -180 || longitude > 180) {
    return { ok: false, error: 'longitude must be between -180 and 180' };
  }
  const round = (v: number) => Math.round(v * 10) / 10 + 0;
  return { ok: true, location: { latitude: round(latitude), longitude: round(longitude) } };
}

/** The location as the firmware prints it ("52.5,13.4"). */
export function formatLocation(location: Location): string {
  return `${location.latitude.toFixed(1)},${location.longitude.toFixed(1)}`;
}
