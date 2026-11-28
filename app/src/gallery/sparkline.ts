import type { Sample } from './types';

/**
 * A battery line for the hub cards (#431): the samples of the last `days`
 * days as SVG polyline points in a `width` × `height` box, 0 % at the bottom.
 * Samples while charging say little (#421) and are left out. Undefined with
 * fewer than two points.
 */
export function sparkPoints(
  samples: Sample[],
  now: number,
  days = 30,
  width = 120,
  height = 28,
): string | undefined {
  const from = now - days * 86_400;
  const points = samples
    .filter((s) => s.at >= from && s.at <= now && s.batPercent !== undefined && !s.charging)
    .sort((a, b) => a.at - b.at)
    .map((s) => {
      const x = ((s.at - from) / (now - from)) * width;
      const y = height - (Math.max(0, Math.min(100, s.batPercent ?? 0)) / 100) * height;
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    });
  return points.length >= 2 ? points.join(' ') : undefined;
}
