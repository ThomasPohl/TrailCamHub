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
   * The latest value is 100 %: charging or full. The gauge reads 100 % while the
   * charger is active, whatever the real level, so there is no usable rate.
   */
  atFull?: boolean;
}

/** A newer value this far below an older one is a discharge; less is gauge noise. */
const NOISE_PERCENT = 1;

/** Fits a line through the points (hours since the first); drain in % per hour. */
function drainOf(points: Sample[]): number {
  const first = points[0];
  if (!first) return 0;
  const n = points.length;
  const xs = points.map((s) => (s.at - first.at) / 3600);
  const ys = points.map((s) => s.batPercent ?? 0);
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

/**
 * Current consumption and an empty-battery forecast from the battery samples
 * (oldest first), extended from the latest value. Needs two values at least an
 * hour apart.
 *
 * Charging is taken out of the measurement:
 * - 100 % is not a measurement: the gauge reads 100 % while charging, whatever
 *   the real level. Such values are dropped, and a latest value of 100 % means
 *   "charging or full" without a forecast.
 * - Only the current discharge counts: going back from the latest value, the
 *   run ends where an older value is lower than a newer one (charged in
 *   between). A least-squares line through that run, at most 24 h, is the rate.
 * - If the latest values rise, the hub is charging: the rate of that rise is
 *   shown, without an empty time.
 */
export function batteryForecast(samples: Sample[]): BatteryForecast | undefined {
  const raw = samples.filter((s) => s.batPercent !== undefined);
  const latest = raw[raw.length - 1];
  if (!latest) return undefined;
  if ((latest.batPercent ?? 0) >= 100) {
    return { drainPerHour: 0, windowS: 0, lastAt: latest.at, atFull: true };
  }
  const points = raw.filter((s) => (s.batPercent ?? 0) < 100);
  const last = points[points.length - 1];
  if (!last) return undefined;

  const prev = points[points.length - 2];
  const charging =
    prev !== undefined && (prev.batPercent ?? 0) < (last.batPercent ?? 0) - NOISE_PERCENT;
  // Walk back to the start of the current run (discharging or charging).
  let startIndex = points.length - 1;
  while (startIndex > 0) {
    const older = points[startIndex - 1];
    const newer = points[startIndex];
    if (!older || !newer) break;
    const delta = (newer.batPercent ?? 0) - (older.batPercent ?? 0);
    if (charging ? delta < NOISE_PERCENT : delta > NOISE_PERCENT) break;
    if (last.at - older.at > FORECAST_WINDOW_S) break;
    startIndex -= 1;
  }
  const run = points.slice(startIndex);
  const first = run[0];
  if (!first || last.at - first.at < 3600) return undefined;

  const drainPerHour = drainOf(run);
  const windowS = last.at - first.at;
  if (drainPerHour < MIN_DRAIN_PER_HOUR) return { drainPerHour, windowS, lastAt: last.at };
  const level = last.batPercent ?? 0;
  return {
    drainPerHour,
    windowS,
    lastAt: last.at,
    emptyAt: last.at + (level / drainPerHour) * 3600,
  };
}
