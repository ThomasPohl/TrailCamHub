/**
 * Does a thumbnail show something that moved? (#442, phase 1)
 *
 * No model: a thumbnail is compared with its neighbours in time from the same
 * camera. The median of those neighbours is the background; what differs from
 * it in one compact patch is an animal, a person or a car. Wind in the grass,
 * a cloud over the sun or sensor noise change many pixels a little or a few
 * pixels scattered, not one patch. The result is a score (the share of the
 * picture the largest patch covers); whether that counts as empty depends on
 * the user's chosen sensitivity, so changing it needs no new analysis.
 *
 * An animal that stays in front of the camera triggers it again and again. If
 * those pictures were the background, the animal would be part of it and every
 * one of them would look empty. So the pictures of the same series (taken
 * shortly one after the other) are never neighbours of each other, however
 * long the series lasts (#456): a picture inside it is compared with pictures
 * before and after the whole series, even far away, or gets no verdict.
 *
 * Pure functions on small grey grids; the browser part that makes the grids
 * from the pictures is `emptyFrames.ts`.
 */

/** The grid a thumbnail is reduced to. */
export const GRID_W = 80;
export const GRID_H = 45;
/**
 * Rows that are compared. The camera prints an info bar across the bottom of
 * every thumbnail (about 9 % of the height, `stamp.ts`); it changes with every
 * picture and is cut off.
 */
export const CROP_H = 40;
/** Changes whenever the analysis changes: older results are made again. */
export const METHOD = 'diff-v4';

/**
 * A pixel counts as changed when it differs from the background by more than
 * its own noise (`NOISE_K` × the spread of the neighbours at that pixel), but
 * at least `MIN_CHANGE` and at most `MAX_CHANGE`. Grass that moves in every
 * picture thus needs a larger change than a still path; the cap keeps a very
 * restless spot from hiding an animal (it is then rather taken for one).
 */
export const MIN_CHANGE = 18;
const MAX_CHANGE = 40;
const NOISE_K = 4.5;
/** Neighbours used for the background, and the fewest that make a judgement possible. */
const MAX_NEIGHBOURS = 9;
export const MIN_NEIGHBOURS = 3;
/** How far (in pictures, outside the own series) the neighbours may be, and how different their brightness may be. */
export const NEIGHBOUR_REACH = 8;
const MAX_BRIGHTNESS_GAP = 50;
/** Pictures less than this far apart (seconds) belong to one series. */
export const SERIES_GAP_S = 10 * 60;
/**
 * At most this many neighbours come from one other series (on each side), so
 * a long series of one animal next to a picture cannot fill its background.
 */
export const MAX_PER_SERIES = 2;
/** Above this share of changed pixels the whole scene changed (light, a moved camera). */
const SCENE_CHANGE = 0.5;
/** A patch must fill this much of its bounding box; thin and scattered changes do not. */
const MIN_FILL = 0.25;
/** Patches smaller than this many pixels are noise. */
const MIN_PATCH = 4;

export interface Frame {
  /** `GRID_W` × `CROP_H` grey values, blurred. */
  gray: Uint8Array;
  /** Mean grey value. */
  mean: number;
}

/** A frame from the RGBA pixels of a `GRID_W` × `GRID_H` picture. */
export function frameFromRgba(rgba: ArrayLike<number>): Frame {
  const grey = new Float32Array(GRID_W * GRID_H);
  for (let i = 0; i < grey.length; i++) {
    grey[i] =
      0.299 * (rgba[i * 4] as number) +
      0.587 * (rgba[i * 4 + 1] as number) +
      0.114 * (rgba[i * 4 + 2] as number);
  }
  // 3 × 3 box blur; the edges use the pixels that exist.
  const gray = new Uint8Array(GRID_W * CROP_H);
  let sum = 0;
  for (let y = 0; y < CROP_H; y++) {
    for (let x = 0; x < GRID_W; x++) {
      let total = 0;
      let count = 0;
      for (let dy = -1; dy <= 1; dy++) {
        for (let dx = -1; dx <= 1; dx++) {
          const xx = x + dx;
          const yy = y + dy;
          // Never from the info bar: its changing text must not bleed into the picture.
          if (xx < 0 || xx >= GRID_W || yy < 0 || yy >= CROP_H) continue;
          total += grey[yy * GRID_W + xx] as number;
          count += 1;
        }
      }
      const value = Math.round(total / count);
      gray[y * GRID_W + x] = value;
      sum += value;
    }
  }
  return { gray, mean: sum / gray.length };
}

/**
 * The times of a camera's pictures (sorted, seconds) as series: each picture
 * gets the index of the first picture of its series. A series is a run of
 * pictures each less than `SERIES_GAP_S` after the one before.
 */
export function seriesOf(times: readonly number[]): number[] {
  const series: number[] = [];
  times.forEach((time, i) => {
    const before = times[i - 1];
    series.push(
      i > 0 && before !== undefined && time - before < SERIES_GAP_S ? (series[i - 1] as number) : i,
    );
  });
  return series;
}

/** When the pictures were taken; without it every picture is its own series. */
export interface Timeline {
  times: readonly number[];
  /** From `seriesOf(times)`. */
  series: readonly number[];
}

/** The positions that may be neighbours of a picture (see `candidateSetsOf`). */
export interface Candidates {
  /** The nearest first, alternately before and after. */
  preferred: number[];
  /** Only used when the preferred ones are too few. */
  spare: number[];
}

/**
 * The positions that may be neighbours of picture `index` out of `count`
 * (sorted by time), beyond the whole series of the picture: up to
 * `NEIGHBOUR_REACH` on each side, at most `MAX_PER_SERIES` of each other
 * series.
 *
 * A long chain of triggers (wind all afternoon) is one series as well. Its
 * far end could be a better background (the light is closer), but an animal
 * that stays as long (grazing deer, cattle) would then be its own background
 * and look empty (#456). Missing an animal is the costly error: without
 * pictures outside the series there is no verdict (`few`) until more come.
 *
 * An empty picture next to a long series of one animal would get mostly
 * pictures of the animal as neighbours; their median contains it, and the
 * empty picture would count as an animal (#457). So the further pictures of a
 * series are only spare, used when there are too few others.
 */
export function candidateSetsOf(count: number, index: number, timeline?: Timeline): Candidates {
  const seriesAt = (i: number) => (timeline ? (timeline.series[i] as number) : i);
  const own = seriesAt(index);
  const side = (dir: number) => {
    const preferred: number[] = [];
    const spare: number[] = [];
    const taken = new Map<number, number>();
    let i = index + dir;
    // A series is a run in time order: skipping it is skipping the run.
    while (i >= 0 && i < count && seriesAt(i) === own) i += dir;
    for (; preferred.length < NEIGHBOUR_REACH && i >= 0 && i < count; i += dir) {
      const series = seriesAt(i);
      const n = taken.get(series) ?? 0;
      if (n < MAX_PER_SERIES) {
        preferred.push(i);
        taken.set(series, n + 1);
      } else if (spare.length < MIN_NEIGHBOURS) {
        spare.push(i);
      }
    }
    return { preferred, spare };
  };
  const before = side(-1);
  const after = side(1);
  const interleave = (a: number[], b: number[]) => {
    const out: number[] = [];
    for (let k = 0; k < Math.max(a.length, b.length); k++) {
      if (a[k] !== undefined) out.push(a[k] as number);
      if (b[k] !== undefined) out.push(b[k] as number);
    }
    return out;
  };
  return {
    preferred: interleave(before.preferred, after.preferred),
    spare: interleave(before.spare, after.spare),
  };
}

/** Every position `candidateSetsOf` may use: the pictures to read for a judgement. */
export function candidatesOf(count: number, index: number, timeline?: Timeline): number[] {
  const { preferred, spare } = candidateSetsOf(count, index, timeline);
  return [...preferred, ...spare];
}

/**
 * The positions of the neighbours of `frames[index]` (sorted by time; a
 * picture that could not be read is `undefined`): the candidates
 * (`candidateSetsOf`) whose brightness is close, so a day picture is not
 * compared with a night one. At most `MAX_NEIGHBOURS`, the nearest first;
 * spare ones only up to `MIN_NEIGHBOURS`.
 */
export function neighbourPositionsOf(
  frames: readonly (Frame | undefined)[],
  index: number,
  timeline?: Timeline,
): number[] {
  const own = frames[index];
  if (!own) return [];
  const { preferred, spare } = candidateSetsOf(frames.length, index, timeline);
  const result: number[] = [];
  const add = (positions: number[], limit: number) => {
    for (const i of positions) {
      if (result.length >= limit) return;
      const other = frames[i];
      if (other && Math.abs(other.mean - own.mean) <= MAX_BRIGHTNESS_GAP) result.push(i);
    }
  };
  add(preferred, MAX_NEIGHBOURS);
  add(spare, MIN_NEIGHBOURS);
  return result;
}

/** The neighbours of `frames[index]` themselves (see `neighbourPositionsOf`). */
export function neighboursOf(
  frames: readonly (Frame | undefined)[],
  index: number,
  timeline?: Timeline,
): Frame[] {
  return neighbourPositionsOf(frames, index, timeline).map((i) => frames[i] as Frame);
}

export interface Measure {
  /** Share of the compared picture that the largest moving patch covers, 0 to 1. */
  score: number;
  /** No judgement: too few neighbours, or the whole scene changed. */
  unsure: boolean;
  /** Why it is unsure: `few` may change when more pictures come, `scene` does not. */
  reason?: 'few' | 'scene';
}

function median(values: number[]): number {
  values.sort((a, b) => a - b);
  const mid = values.length >> 1;
  return values.length % 2
    ? (values[mid] as number)
    : ((values[mid - 1] as number) + (values[mid] as number)) / 2;
}

/** Area of the largest compact patch in a mask of `GRID_W` × `CROP_H`. */
function largestPatch(mask: Uint8Array): number {
  const seen = new Uint8Array(mask.length);
  let best = 0;
  for (let start = 0; start < mask.length; start++) {
    if (!mask[start] || seen[start]) continue;
    const stack = [start];
    seen[start] = 1;
    let area = 0;
    let minX = GRID_W;
    let maxX = 0;
    let minY = CROP_H;
    let maxY = 0;
    while (stack.length > 0) {
      const at = stack.pop() as number;
      const x = at % GRID_W;
      const y = (at - x) / GRID_W;
      area += 1;
      minX = Math.min(minX, x);
      maxX = Math.max(maxX, x);
      minY = Math.min(minY, y);
      maxY = Math.max(maxY, y);
      const next = [
        x > 0 ? at - 1 : -1,
        x < GRID_W - 1 ? at + 1 : -1,
        y > 0 ? at - GRID_W : -1,
        y < CROP_H - 1 ? at + GRID_W : -1,
      ];
      for (const n of next) {
        if (n >= 0 && mask[n] && !seen[n]) {
          seen[n] = 1;
          stack.push(n);
        }
      }
    }
    const box = (maxX - minX + 1) * (maxY - minY + 1);
    if (area >= MIN_PATCH && area / box >= MIN_FILL) best = Math.max(best, area);
  }
  return best;
}

/** Compares a frame with its neighbours (see `neighboursOf`). */
export function measure(frame: Frame, neighbours: readonly Frame[]): Measure {
  if (neighbours.length < MIN_NEIGHBOURS) return { score: 0, unsure: true, reason: 'few' };
  const size = frame.gray.length;
  const column: number[] = new Array<number>(neighbours.length);
  const spread: number[] = new Array<number>(neighbours.length);
  const mask = new Uint8Array(size);
  let changed = 0;
  for (let i = 0; i < size; i++) {
    // A cloud or the camera's exposure moves every pixel alike: each picture
    // is taken relative to its own mean brightness.
    for (let k = 0; k < neighbours.length; k++) {
      const n = neighbours[k] as Frame;
      column[k] = (n.gray[i] as number) - n.mean;
    }
    const background = median(column);
    for (let k = 0; k < neighbours.length; k++) {
      spread[k] = Math.abs((column[k] as number) - background);
    }
    const threshold = Math.min(MAX_CHANGE, Math.max(MIN_CHANGE, NOISE_K * median(spread)));
    const diff = (frame.gray[i] as number) - frame.mean - background;
    if (Math.abs(diff) > threshold) {
      mask[i] = 1;
      changed += 1;
    }
  }
  if (changed / size > SCENE_CHANGE) return { score: 0, unsure: true, reason: 'scene' };
  return { score: largestPatch(mask) / size, unsure: false };
}

export type Sensitivity = 'careful' | 'normal' | 'strict';

/**
 * From this share of the picture a moving patch counts as an animal. Careful
 * marks fewer pictures as empty (it takes small patches seriously, about 8
 * grid pixels), strict more. These values are a first guess; they are not
 * tuned on garden pictures.
 */
export const PATCH_SHARE: Record<Sensitivity, number> = {
  careful: 0.0025,
  normal: 0.012,
  strict: 0.03,
};

export type Verdict = 'empty' | 'animal' | 'unsure';

export function verdictFor(measure: Measure, sensitivity: Sensitivity): Verdict {
  if (measure.unsure) return 'unsure';
  return measure.score >= PATCH_SHARE[sensitivity] ? 'animal' : 'empty';
}
