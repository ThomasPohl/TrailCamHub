import { describe, expect, it } from 'vitest';
import {
  CROP_H,
  frameFromRgba,
  GRID_H,
  GRID_W,
  measure,
  MIN_NEIGHBOURS,
  neighboursOf,
  PATCH_SHARE,
  verdictFor,
  type Frame,
} from './emptiness';

/** A small deterministic noise source. */
function noise(seed: number) {
  let state = seed;
  return () => {
    state = (state * 1_664_525 + 1_013_904_223) % 4_294_967_296;
    return state / 4_294_967_296 - 0.5;
  };
}

interface Scene {
  seed: number;
  /** Added to every pixel (a cloud, the exposure). */
  light?: number;
  /** A bright or dark rectangle: x, y, width, height, difference. */
  patch?: [number, number, number, number, number];
  /** Isolated changed pixels (wind in the grass). */
  specks?: number;
  base?: number;
  /** The camera's info bar at the bottom: changes with every picture. */
  bar?: number;
}

/** A frame of a meadow: a gradient with a little noise, plus what the scene adds. */
function frame(scene: Scene): Frame {
  const rand = noise(scene.seed);
  const rgba = new Uint8ClampedArray(GRID_W * GRID_H * 4);
  const speck = new Set<number>();
  const pick = noise(scene.seed * 7 + 1);
  for (let i = 0; i < (scene.specks ?? 0); i++) {
    const x = 2 + Math.floor((pick() + 0.5) * (GRID_W - 4));
    const y = 2 + Math.floor((pick() + 0.5) * (CROP_H - 4));
    // Every second pixel, so no two touch.
    speck.add((y - (y % 2)) * GRID_W + (x - (x % 2)));
  }
  for (let y = 0; y < GRID_H; y++) {
    for (let x = 0; x < GRID_W; x++) {
      let value = (scene.base ?? 110) + y * 0.6 + rand() * 6 + (scene.light ?? 0);
      const p = scene.patch;
      if (p && x >= p[0] && x < p[0] + p[2] && y >= p[1] && y < p[1] + p[3]) value += p[4];
      if (speck.has(y * GRID_W + x)) value += 120;
      if (y >= CROP_H) value = scene.bar ?? 20 + (scene.seed % 7) * 30;
      const at = (y * GRID_W + x) * 4;
      rgba[at] = rgba[at + 1] = rgba[at + 2] = Math.max(0, Math.min(255, value));
      rgba[at + 3] = 255;
    }
  }
  return frameFromRgba(rgba);
}

/** Ten empty pictures of the same place. */
const meadow = (n = 10) => Array.from({ length: n }, (_, i) => frame({ seed: i + 1 }));

describe('measure', () => {
  it('finds nothing in an empty picture, whatever the info bar says', () => {
    const frames = meadow();
    const subject = frame({ seed: 50, bar: 250 });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(result.unsure).toBe(false);
    expect(result.score).toBe(0);
    expect(verdictFor(result, 'strict')).toBe('empty');
  });

  it('finds a compact patch: an animal', () => {
    const frames = meadow();
    const subject = frame({ seed: 51, patch: [30, 15, 12, 10, 70] });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(result.score).toBeGreaterThan(PATCH_SHARE.strict);
    expect(verdictFor(result, 'careful')).toBe('animal');
    expect(verdictFor(result, 'strict')).toBe('animal');
  });

  it('finds a dark animal as well', () => {
    const frames = meadow();
    const subject = frame({ seed: 52, patch: [10, 20, 14, 9, -80] });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(verdictFor(result, 'normal')).toBe('animal');
  });

  it('is not fooled by a cloud: the whole picture gets brighter', () => {
    const frames = meadow();
    const subject = frame({ seed: 53, light: 45 });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(result.unsure).toBe(false);
    expect(verdictFor(result, 'careful')).toBe('empty');
  });

  it('is not fooled by scattered pixels such as moving grass', () => {
    const frames = meadow();
    const subject = frame({ seed: 54, specks: 25 });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(verdictFor(result, 'careful')).toBe('empty');
  });

  it('gives no verdict without enough neighbours', () => {
    const subject = frame({ seed: 55 });
    const few = meadow(MIN_NEIGHBOURS - 1);
    expect(measure(subject, few)).toEqual({ score: 0, unsure: true });
    expect(verdictFor({ score: 0, unsure: true }, 'normal')).toBe('unsure');
  });

  it('gives no verdict when the whole scene changed', () => {
    const frames = meadow();
    const moved = frame({ seed: 56, patch: [0, 0, GRID_W, CROP_H, 90], light: 0 });
    // The patch covers everything: shift compensation removes it, so use a half-picture change.
    const half = frame({ seed: 57, patch: [0, 0, GRID_W / 2 + 10, CROP_H, 90] });
    const result = measure(half, frames);
    expect(result.unsure).toBe(true);
    expect(measure(moved, frames).unsure).toBe(false);
  });
});

describe('neighboursOf', () => {
  it('takes the nearest pictures first, on both sides, at most nine', () => {
    const frames = meadow(30);
    expect(neighboursOf(frames, 15)).toHaveLength(9);
    expect(neighboursOf(frames, 0).length).toBeGreaterThanOrEqual(MIN_NEIGHBOURS);
  });

  it('leaves out pictures of another brightness (night against day)', () => {
    const day = meadow(6);
    const night = Array.from({ length: 6 }, (_, i) => frame({ seed: 100 + i, base: 30 }));
    const frames = [...day, ...night];
    expect(neighboursOf(frames, 2).every((f) => f.mean > 80)).toBe(true);
    expect(neighboursOf(frames, 8).every((f) => f.mean < 80)).toBe(true);
  });

  it('is empty for an index that does not exist', () => {
    expect(neighboursOf(meadow(3), 9)).toEqual([]);
  });
});
