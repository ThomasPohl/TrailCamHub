import type { Sample } from './types';

/** Window the current consumption is measured over, in seconds. */
export const FORECAST_WINDOW_S = 24 * 3600;
/** Below this net drain in %/h the battery counts as not discharging. */
const MIN_DRAIN_PER_HOUR = 0.05;

export interface BatteryForecast {
  /** Net drain in % per hour over the window; negative while charging. */
  drainPerHour: number;
  /** Unix seconds when the battery runs empty; absent if it is not discharging. */
  emptyAt?: number;
  /** Seconds covered by the measurement. */
  windowS: number;
  /** Time of the latest value, Unix seconds; the forecast starts there. */
  lastAt: number;
  /**
   * The latest value comes from a charging cell (flag from the hub, #421): the
   * gauge reads 100 % then, whatever the real level. No rate, no empty time.
   */
  charging?: boolean;
  /**
   * The latest value is 100 % from firmware without the charging flag: charging
   * or full, no usable rate.
   */
  atFull?: boolean;
}

/**
 * A newer value this far below an older one is a discharge; less is gauge noise.
 * Right after a full charge the gauge wanders by two points (83, 85, 83, 85).
 */
const NOISE_PERCENT = 2;

/**
 * Two values further apart than this have no data between them: the hub was
 * off or did not report (#472). The gap counts neither as running time nor as
 * drain; a hub checks in every 30 min to 1.5 h while it runs.
 */
export const GAP_S = 3 * 3600;

/**
 * Fits a line through the points; drain in % per hour. Gaps (see `GAP_S`) are
 * cut out: later values move forward in time and up by what the level lost
 * during the gap, so only running time counts.
 */
function drainOf(points: Sample[]): number {
  const first = points[0];
  if (!first) return 0;
  const n = points.length;
  let skippedS = 0;
  let skippedPercent = 0;
  const xs: number[] = [];
  const ys: number[] = [];
  points.forEach((s, i) => {
    const before = points[i - 1];
    if (before && s.at - before.at > GAP_S) {
      skippedS += s.at - before.at;
      skippedPercent += (before.batPercent ?? 0) - (s.batPercent ?? 0);
    }
    xs.push((s.at - first.at - skippedS) / 3600);
    ys.push((s.batPercent ?? 0) + skippedPercent);
  });
  const meanX = xs.reduce((a, b) => a + b, 0) / n;
  const meanY = ys.reduce((a, b) => a + b, 0) / n;
  let cov = 0;
  let varX = 0;
  xs.forEach((x, i) => {
    cov += (x - meanX) * ((ys[i] ?? 0) - meanY);
    varX += (x - meanX) ** 2;
  });
  return varX === 0 ? 0 : -cov / varX;
}

/** Seconds of running time within the points: the time span without the gaps. */
function runningSeconds(points: Sample[]): number {
  let total = 0;
  points.forEach((s, i) => {
    const before = points[i - 1];
    if (before && s.at - before.at <= GAP_S) total += s.at - before.at;
  });
  return total;
}

/**
 * A value that says little about the level: flagged as charging by the hub
 * (#421). Older firmware sends no flag; its 100 % is then taken as "charging
 * or full", since the gauge reads 100 % while charging.
 */
function unreliable(s: Sample): boolean {
  return s.charging ?? (s.batPercent ?? 0) >= 100;
}

/**
 * Current consumption and an empty-battery forecast from the battery samples
 * (oldest first), extended from the latest value. Needs two values at least an
 * hour apart.
 *
 * Charging is taken out of the measurement:
 * - Values the hub flags as charging are dropped, and so are 100 % values
 *   without a flag. A latest value like that means "charging or full" without
 *   a forecast.
 * - Only the current discharge counts: going back from the latest value, the
 *   run ends at a dropped value or where an older value is lower than a newer
 *   one (charged in between). A least-squares line through that run, at most
 *   24 h of running time, is the rate.
 * - Gaps of more than `GAP_S` (hub off or silent) are cut out: they are neither
 *   running time nor drain (#472).
 * - If the latest values rise, the hub is charging: the rate of that rise is
 *   shown, without an empty time.
 */
export function batteryForecast(samples: Sample[]): BatteryForecast | undefined {
  const raw = samples.filter((s) => s.batPercent !== undefined);
  const latest = raw[raw.length - 1];
  if (!latest) return undefined;
  if (unreliable(latest)) {
    return {
      drainPerHour: 0,
      windowS: 0,
      lastAt: latest.at,
      ...(latest.charging ? { charging: true } : { atFull: true }),
    };
  }
  const points = raw
    .map((sample, index) => ({ sample, index }))
    .filter((p) => !unreliable(p.sample));
  const lastPoint = points[points.length - 1];
  if (!lastPoint) return undefined;
  const last = lastPoint.sample;

  const prev = points[points.length - 2]?.sample;
  const charging =
    prev !== undefined && (prev.batPercent ?? 0) < (last.batPercent ?? 0) - NOISE_PERCENT;
  // Walk back to the start of the current run (discharging or charging).
  let startIndex = points.length - 1;
  let runningS = 0;
  while (startIndex > 0) {
    const older = points[startIndex - 1];
    const newer = points[startIndex];
    if (!older || !newer) break;
    // A dropped value in between: the hub charged there.
    if (newer.index - older.index > 1) break;
    const delta = (newer.sample.batPercent ?? 0) - (older.sample.batPercent ?? 0);
    if (charging ? delta < NOISE_PERCENT : delta > NOISE_PERCENT) break;
    const step = newer.sample.at - older.sample.at;
    const running = runningS + (step > GAP_S ? 0 : step);
    if (running > FORECAST_WINDOW_S) break;
    runningS = running;
    startIndex -= 1;
  }
  const run = points.slice(startIndex).map((p) => p.sample);
  const first = run[0];
  const windowS = runningSeconds(run);
  if (!first || windowS < 3600) return undefined;

  const drainPerHour = drainOf(run);
  if (drainPerHour < MIN_DRAIN_PER_HOUR) return { drainPerHour, windowS, lastAt: last.at };
  const level = last.batPercent ?? 0;
  return {
    drainPerHour,
    windowS,
    lastAt: last.at,
    emptyAt: last.at + (level / drainPerHour) * 3600,
  };
}

/** A camera that reports a few steps (the TC100: 100, 80, 20, 0) has no rate. */
const LEVEL_STEPS = [0, 20, 40, 60, 80, 100];
const MAX_LEVEL_VALUES = 4;
/** At or below this the camera battery counts as low (the fault threshold, #265). */
export const LEVEL_LOW_PERCENT = 25;
/** The lowest value of this much time counts, so a camera on a step edge does not flap. */
export const LEVEL_WINDOW_S = 12 * 3600;
/** A low phase ends after this long without a low value. */
const LOW_PHASE_END_S = 24 * 3600;

export interface BatteryLevel {
  /** Lowest value of the last `LEVEL_WINDOW_S`, or "charging" for a flagged latest value. */
  state: 'good' | 'low' | 'empty' | 'charging';
  /** Time of the first low value of the current low phase, Unix seconds. */
  lowSince?: number;
  /** Time of the latest value, Unix seconds. */
  lastAt: number;
}

/** True if the values look like steps and not like a percentage (#472). */
export function reportsLevels(samples: Sample[]): boolean {
  const values = new Set(
    samples.flatMap((s) => (s.batPercent === undefined ? [] : [s.batPercent])),
  );
  return (
    values.size > 0 &&
    values.size <= MAX_LEVEL_VALUES &&
    [...values].every((v) => LEVEL_STEPS.includes(v))
  );
}

/**
 * Battery state of a camera that reports steps: "good", "low since …" or
 * "empty", from the lowest value of the last 12 h. A low phase starts with the
 * first low value and goes on while low values come within 24 h of each other;
 * a charged value (flagged, or an unflagged 100 %) ends it. Undefined without
 * values.
 */
export function batteryLevel(samples: Sample[]): BatteryLevel | undefined {
  const points = samples.filter((s) => s.batPercent !== undefined);
  const latest = points[points.length - 1];
  if (!latest) return undefined;
  const charged = (s: Sample) => s.charging ?? (s.batPercent ?? 0) >= 100;
  if (latest.charging === true) return { state: 'charging', lastAt: latest.at };

  let phaseStart: number | undefined;
  let lastLowAt = 0;
  let chargedAt = -Infinity;
  for (const s of points) {
    if (charged(s)) {
      phaseStart = undefined;
      chargedAt = s.at;
    } else if ((s.batPercent ?? 0) <= LEVEL_LOW_PERCENT) {
      if (phaseStart === undefined || s.at - lastLowAt > LOW_PHASE_END_S) phaseStart = s.at;
      lastLowAt = s.at;
    }
  }
  // Only values after the last charge say anything about the level now.
  const since = Math.max(latest.at - LEVEL_WINDOW_S, chargedAt);
  const recent = points.filter((s) => s.at > since).map((s) => s.batPercent ?? 0);
  const lowest = recent.length > 0 ? Math.min(...recent) : 100;
  const state = lowest <= 0 ? 'empty' : lowest <= LEVEL_LOW_PERCENT ? 'low' : 'good';
  return {
    state,
    lastAt: latest.at,
    ...(state !== 'good' && phaseStart !== undefined ? { lowSince: phaseStart } : {}),
  };
}
