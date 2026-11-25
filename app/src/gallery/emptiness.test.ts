import { describe, expect, it } from 'vitest';
import {
  candidatesOf,
  CROP_H,
  frameFromRgba,
  GRID_H,
  GRID_W,
  measure,
  MIN_NEIGHBOURS,
  neighboursOf,
  PATCH_SHARE,
  SERIES_GAP_S,
  seriesOf,
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
    expect(measure(subject, few)).toEqual({ score: 0, unsure: true, reason: 'few' });
    expect(verdictFor({ score: 0, unsure: true }, 'normal')).toBe('unsure');
  });

  it('gives no verdict when the whole scene changed', () => {
    const frames = meadow();
    const moved = frame({ seed: 56, patch: [0, 0, GRID_W, CROP_H, 90], light: 0 });
    // The patch covers everything: shift compensation removes it, so use a half-picture change.
    const half = frame({ seed: 57, patch: [0, 0, GRID_W / 2 + 10, CROP_H, 90] });
    const result = measure(half, frames);
    expect(result).toMatchObject({ unsure: true, reason: 'scene' });
    expect(measure(moved, frames).unsure).toBe(false);
  });

  it('finds an animal with little contrast (night, brown on brown)', () => {
    const frames = meadow();
    const subject = frame({ seed: 58, patch: [30, 15, 12, 10, 24] });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(verdictFor(result, 'careful')).toBe('animal');
  });

  it('finds a small animal far away when careful', () => {
    const frames = meadow();
    const subject = frame({ seed: 59, patch: [40, 20, 3, 3, 80] });
    const result = measure(subject, neighboursOf([...frames, subject], frames.length));
    expect(verdictFor(result, 'careful')).toBe('animal');
  });
});

/** `n` pictures, `gap` seconds apart, starting at `start`. */
const timesOf = (n: number, gap: number, start = 1_800_000_000) =>
  Array.from({ length: n }, (_, i) => start + i * gap);

describe('seriesOf', () => {
  it('puts pictures taken shortly one after the other into one series', () => {
    const times = [0, 60, 120, 120 + SERIES_GAP_S, 120 + SERIES_GAP_S + 30, 5 * SERIES_GAP_S];
    expect(seriesOf(times)).toEqual([0, 0, 0, 3, 3, 5]);
    expect(seriesOf([])).toEqual([]);
  });
});

describe('a series of one animal (#442 review)', () => {
  /** Hours apart, empty; with a deer standing in pictures 4 to 9, a minute apart. */
  function scene(moves: boolean) {
    const times: number[] = [];
    const frames: Frame[] = [];
    for (let i = 0; i < 14; i++) {
      const deer = i >= 4 && i < 10;
      const empty = (i < 4 ? 1_800_000_000 : 1_800_200_000) + i * 7_200;
      times.push(deer ? 1_800_100_000 + (i - 4) * 60 : empty);
      const x = moves ? 24 + (i - 4) * 2 : 30;
      frames.push(frame({ seed: i + 1, ...(deer && { patch: [x, 15, 12, 10, 70] }) }));
    }
    const order = times.map((t, i) => [t, i] as const).sort((a, b) => a[0] - b[0]);
    const sortedTimes = order.map(([t]) => t);
    return {
      frames: order.map(([, i]) => frames[i] as Frame),
      timeline: { times: sortedTimes, series: seriesOf(sortedTimes) },
      deer: order.flatMap(([, i], at) => (i >= 4 && i < 10 ? [at] : [])),
    };
  }

  it('does not call a deer that stays in front of the camera empty', () => {
    const { frames, timeline, deer } = scene(false);
    expect(deer).toHaveLength(6);
    for (const at of deer) {
      const result = measure(frames[at] as Frame, neighboursOf(frames, at, timeline));
      expect(verdictFor(result, 'strict')).toBe('animal');
    }
  });

  it('would call it empty if the series were its own background', () => {
    // The reason for the series rule: without it the deer is in the median.
    const { frames, deer } = scene(false);
    const at = deer[2] as number;
    expect(verdictFor(measure(frames[at] as Frame, neighboursOf(frames, at)), 'careful')).toBe(
      'empty',
    );
  });

  it('does not call a deer empty that stays for an hour (#456)', () => {
    // Empty pictures hours apart around a deer that triggers the camera every
    // minute for an hour: the middle of its stay is more than 30 minutes from
    // both ends, which used to make the ends its background.
    const times: number[] = [];
    const frames: Frame[] = [];
    for (let i = 0; i < 4; i++) {
      times.push(1_800_000_000 + i * 7_200);
      frames.push(frame({ seed: i + 1 }));
    }
    for (let i = 0; i < 60; i++) {
      times.push(1_800_100_000 + i * 60);
      frames.push(frame({ seed: 10 + i, patch: [30, 15, 12, 10, 70] }));
    }
    for (let i = 0; i < 4; i++) {
      times.push(1_800_200_000 + i * 7_200);
      frames.push(frame({ seed: 80 + i }));
    }
    const timeline = { times, series: seriesOf(times) };
    for (const at of [4, 20, 34, 50, 63]) {
      const result = measure(frames[at] as Frame, neighboursOf(frames, at, timeline));
      expect(verdictFor(result, 'strict')).toBe('animal');
    }
  });

  it('gives no verdict inside a long series without pictures around it', () => {
    const times = Array.from({ length: 60 }, (_, i) => 1_800_100_000 + i * 60);
    const frames = times.map((_, i) => frame({ seed: i + 1, patch: [30, 15, 12, 10, 70] }));
    const timeline = { times, series: seriesOf(times) };
    const result = measure(frames[30] as Frame, neighboursOf(frames, 30, timeline));
    expect(result).toMatchObject({ unsure: true, reason: 'few' });
  });

  it('finds a deer that moves a little, too', () => {
    const { frames, timeline, deer } = scene(true);
    for (const at of deer) {
      const result = measure(frames[at] as Frame, neighboursOf(frames, at, timeline));
      expect(verdictFor(result, 'careful')).toBe('animal');
    }
  });
});

describe('candidatesOf', () => {
  it('skips the own series and takes the nearest on both sides', () => {
    const times = [
      ...timesOf(5, 3_600),
      ...timesOf(4, 60, 1_800_100_000),
      ...timesOf(5, 3_600, 1_800_200_000),
    ];
    const timeline = { times, series: seriesOf(times) };
    const picks = candidatesOf(times.length, 6, timeline);
    expect(picks.some((i) => i >= 5 && i <= 8)).toBe(false);
    expect(picks.slice(0, 2)).toEqual([4, 9]);
  });

  it('never takes a picture of its own series, however long the series is (#456)', () => {
    // Wind all afternoon: a trigger every five minutes for four hours, nothing else.
    const times = timesOf(48, 300);
    const timeline = { times, series: seriesOf(times) };
    expect(candidatesOf(times.length, 24, timeline)).toEqual([]);
  });

  it('takes pictures before and after a long chain, even far away', () => {
    const times = [
      ...timesOf(5, 3_600, 1_700_000_000),
      ...timesOf(48, 300, 1_800_000_000),
      ...timesOf(3, 3_600, 1_900_000_000),
    ];
    const timeline = { times, series: seriesOf(times) };
    const picks = candidatesOf(times.length, 30, timeline);
    expect([...picks].sort((a, b) => a - b)).toEqual([0, 1, 2, 3, 4, 53, 54, 55]);
    expect(picks.slice(0, 2)).toEqual([4, 53]);
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

  it('skips pictures that could not be read', () => {
    const frames: (Frame | undefined)[] = meadow(10);
    frames[4] = undefined;
    frames[6] = undefined;
    const picked = neighboursOf(frames, 5);
    // Nine candidates (five before, four after), two of them unreadable.
    expect(picked).toHaveLength(7);
    expect(picked.every((f) => f !== undefined)).toBe(true);
  });
});
