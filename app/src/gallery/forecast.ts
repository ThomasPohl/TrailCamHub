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
}

/**
 * Current consumption and an empty-battery forecast from the battery samples
 * (oldest first): a least-squares line through the last 24 h, extended from
 * the latest value. Needs two values at least an hour apart.
 */
export function batteryForecast(samples: Sample[]): BatteryForecast | undefined {
  const points = samples.filter((s) => s.batPercent !== undefined);
  const last = points[points.length - 1];
  if (!last) return undefined;
  const recent = points.filter((s) => s.at >= last.at - FORECAST_WINDOW_S);
  const first = recent[0];
  if (!first || last.at - first.at < 3600) return undefined;
  const n = recent.length;
  const xs = recent.map((s) => (s.at - first.at) / 3600);
  const ys = recent.map((s) => s.batPercent ?? 0);
  const meanX = xs.reduce((a, b) => a + b, 0) / n;
  const meanY = ys.reduce((a, b) => a + b, 0) / n;
  let cov = 0;
  let varX = 0;
  xs.forEach((x, i) => {
    cov += (x - meanX) * ((ys[i] ?? 0) - meanY);
    varX += (x - meanX) ** 2;
  });
  const drainPerHour = varX === 0 ? 0 : -cov / varX;
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
