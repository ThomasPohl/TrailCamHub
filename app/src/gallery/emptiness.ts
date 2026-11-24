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
export const METHOD = 'diff-v1';

/** A pixel counts as changed when it differs from the background by more than this. */
const CHANGE = 30;
/** Neighbours used for the background, and the fewest that make a judgement possible. */
const MAX_NEIGHBOURS = 9;
export const MIN_NEIGHBOURS = 3;
/** How far (in pictures) the neighbours may be, and how different their brightness may be. */
export const NEIGHBOUR_REACH = 8;
const MAX_BRIGHTNESS_GAP = 50;
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
 * The neighbours of `frames[index]` (sorted by time): the pictures within
 * `NEIGHBOUR_REACH` places whose brightness is close, so a day picture is not
 * compared with a night one. At most `MAX_NEIGHBOURS`, the nearest first.
 */
export function neighboursOf(frames: readonly Frame[], index: number): Frame[] {
  const own = frames[index];
  if (!own) return [];
  const result: Frame[] = [];
  for (let step = 1; step <= NEIGHBOUR_REACH && result.length < MAX_NEIGHBOURS; step++) {
    for (const i of [index - step, index + step]) {
      const other = frames[i];
      if (other && Math.abs(other.mean - own.mean) <= MAX_BRIGHTNESS_GAP) result.push(other);
    }
  }
  return result.slice(0, MAX_NEIGHBOURS);
}

export interface Measure {
  /** Share of the compared picture that the largest moving patch covers, 0 to 1. */
  score: number;
  /** No judgement: too few neighbours, or the whole scene changed. */
  unsure: boolean;
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
  if (neighbours.length < MIN_NEIGHBOURS) return { score: 0, unsure: true };
  const size = frame.gray.length;
  const background = new Float32Array(size);
  const meanBackground = neighbours.reduce((sum, n) => sum + n.mean, 0) / neighbours.length;
  const column: number[] = new Array<number>(neighbours.length);
  for (let i = 0; i < size; i++) {
    for (let k = 0; k < neighbours.length; k++)
      column[k] = (neighbours[k] as Frame).gray[i] as number;
    background[i] = median(column);
  }
  // A cloud or the camera's exposure moves every pixel alike: take that out.
  const shift = frame.mean - meanBackground;
  const mask = new Uint8Array(size);
  let changed = 0;
  for (let i = 0; i < size; i++) {
    const diff = (frame.gray[i] as number) - (background[i] as number) - shift;
    if (Math.abs(diff) > CHANGE) {
      mask[i] = 1;
      changed += 1;
    }
  }
  if (changed / size > SCENE_CHANGE) return { score: 0, unsure: true };
  return { score: largestPatch(mask) / size, unsure: false };
}

export type Sensitivity = 'careful' | 'normal' | 'strict';

/**
 * From this share of the picture a moving patch counts as an animal. Careful
 * marks fewer pictures as empty (it takes small patches seriously), strict
 * more. These values are a first guess; they are not tuned on garden pictures.
 */
export const PATCH_SHARE: Record<Sensitivity, number> = {
  careful: 0.004,
  normal: 0.012,
  strict: 0.03,
};

export type Verdict = 'empty' | 'animal' | 'unsure';

export function verdictFor(measure: Measure, sensitivity: Sensitivity): Verdict {
  if (measure.unsure) return 'unsure';
  return measure.score >= PATCH_SHARE[sensitivity] ? 'animal' : 'empty';
}
