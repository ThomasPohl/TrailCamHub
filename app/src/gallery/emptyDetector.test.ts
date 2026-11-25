import { describe, expect, it, vi } from 'vitest';
import {
  analyseCamera,
  memoryEmptyStore,
  needsAnalysis,
  RETRY_UNREADABLE_S,
  verdictOf,
  type Detected,
} from './emptyDetector';
import { CROP_H, frameFromRgba, GRID_H, GRID_W, METHOD, type Frame } from './emptiness';
import type { Recording } from './types';

/** A grey picture with a little variation; with `patch` a bright rectangle in it. */
function grey(seed: number, patch?: [number, number, number, number]): Frame {
  const rgba = new Uint8ClampedArray(GRID_W * GRID_H * 4);
  for (let y = 0; y < GRID_H; y++) {
    for (let x = 0; x < GRID_W; x++) {
      let value = 100 + ((x * 7 + y * 13 + seed * 5) % 5);
      if (
        patch &&
        x >= patch[0] &&
        x < patch[0] + patch[2] &&
        y >= patch[1] &&
        y < patch[1] + patch[3]
      ) {
        value += 90;
      }
      if (y >= CROP_H) value = 250 - seed;
      const at = (y * GRID_W + x) * 4;
      rgba[at] = rgba[at + 1] = rgba[at + 2] = value;
      rgba[at + 3] = 255;
    }
  }
  return frameFromRgba(rgba);
}

const recording = (n: number, camId = 'CAM'): Recording =>
  ({
    camId,
    mediaId: n,
    mediaTime: 1_800_000_000 + n * 600,
    thumbUrl: `thumb-${camId}-${n}`,
  }) as Recording;

const recordings = (count: number) => Array.from({ length: count }, (_, i) => recording(i));

/** Picture 6 shows an animal, the others are empty. */
const loader = () =>
  vi.fn(async (url: string) => {
    const n = Number(url.split('-')[2]);
    return grey(n, n === 6 ? [30, 12, 14, 10] : undefined);
  });

describe('analyseCamera', () => {
  it('scores every recording against its neighbours', async () => {
    const loadFrame = loader();
    const { results } = await analyseCamera(recordings(12), {
      loadFrame,
      known: new Map(),
      now: 1_800_100_000,
    });
    expect(results.size).toBe(12);
    const animal = results.get('CAM/6') as Detected;
    expect(animal.score).toBeGreaterThan(0.03);
    expect(animal).toMatchObject({ unsure: false, method: METHOD, at: 1_800_100_000 });
    expect(verdictOf(animal, 'strict')).toBe('animal');
    expect(verdictOf(results.get('CAM/2'), 'careful')).toBe('empty');
    expect(loadFrame).toHaveBeenCalledTimes(12);
  });

  it('keeps what is known and reads only what the new pictures need', async () => {
    const all = recordings(14);
    const first = await analyseCamera(all.slice(0, 12), {
      loadFrame: loader(),
      known: new Map(),
      now: 1,
    });
    const loadFrame = loader();
    const second = await analyseCamera(all, { loadFrame, known: first.results, now: 2 });
    // The two new ones, and the three newest before them, which had fewer
    // than three neighbours after them (#458).
    expect([...second.results.keys()].sort()).toEqual([
      'CAM/10',
      'CAM/11',
      'CAM/12',
      'CAM/13',
      'CAM/9',
    ]);
    // Those five and the eight before the first of them, as neighbours.
    expect(loadFrame).toHaveBeenCalledTimes(13);
    expect(loadFrame).not.toHaveBeenCalledWith('thumb-CAM-0');
  });

  it('does nothing when every result is known', async () => {
    const known = new Map<string, Detected>(
      recordings(5).map((r) => [
        `CAM/${r.mediaId}`,
        { score: 0, unsure: false, method: METHOD, at: 1 },
      ]),
    );
    const loadFrame = loader();
    const { results } = await analyseCamera(recordings(5), { loadFrame, known, now: 2 });
    expect(results.size).toBe(0);
    expect(loadFrame).not.toHaveBeenCalled();
  });

  it('is unsure with too few pictures, and tries again when more have come', async () => {
    const few = await analyseCamera(recordings(3), {
      loadFrame: loader(),
      known: new Map(),
      now: 1,
    });
    expect([...few.results.values()].every((d) => d.unsure && d.reason === 'few')).toBe(true);
    expect(few.results.get('CAM/0')?.pool).toBe(3);
    const more = await analyseCamera(recordings(9), {
      loadFrame: loader(),
      known: few.results,
      now: 2,
    });
    expect(more.results.get('CAM/0')?.unsure).toBe(false);
    // The ones judged before are only looked at again if they were unsure.
    expect(more.results.size).toBe(9);
  });

  it('does not touch a recording the user corrected, even if it was unsure', async () => {
    const known = new Map<string, Detected>([
      ['CAM/0', { score: 0, unsure: true, method: METHOD, at: 1, corrected: 'empty' }],
    ]);
    const { results } = await analyseCamera(recordings(9), { loadFrame: loader(), known, now: 2 });
    expect(results.has('CAM/0')).toBe(false);
    expect(results.size).toBe(8);
  });

  it('leaves out pictures that cannot be read', async () => {
    const loadFrame = vi.fn(async (url: string) =>
      url === 'thumb-CAM-3' ? undefined : grey(Number(url.split('-')[2])),
    );
    const { results } = await analyseCamera(recordings(10), {
      loadFrame,
      known: new Map(),
      now: 1,
    });
    // Kept as unreadable, so it is not read again at every new recording.
    expect(results.get('CAM/3')).toMatchObject({ unsure: true, reason: 'unreadable' });
    expect(verdictOf(results.get('CAM/3'), 'strict')).toBe('unsure');
    expect(results.size).toBe(10);
  });

  it('marks a deer that triggers the camera again and again as an animal', async () => {
    // Empty pictures hours apart, and a deer in 6 pictures a minute apart.
    const list = Array.from({ length: 14 }, (_, i) => ({
      ...recording(i),
      mediaTime:
        i >= 4 && i < 10
          ? 1_800_100_000 + (i - 4) * 60
          : (i < 4 ? 1_800_000_000 : 1_800_200_000) + i * 7_200,
    }));
    const loadFrame = vi.fn(async (url: string) => {
      const n = Number(url.split('-')[2]);
      return grey(n, n >= 4 && n < 10 ? [30, 12, 14, 10] : undefined);
    });
    const { results } = await analyseCamera(list, { loadFrame, known: new Map(), now: 1 });
    for (let n = 4; n < 10; n++)
      expect(verdictOf(results.get(`CAM/${n}`), 'strict')).toBe('animal');
    // The empty ones around it get at most two pictures of the deer as
    // neighbours (#457), so the deer is not in their background.
    for (const n of [3, 10]) expect(verdictOf(results.get(`CAM/${n}`), 'careful')).toBe('empty');
  });

  it('judges the newest recordings again once later ones exist (#458)', async () => {
    const all = recordings(20);
    const first = await analyseCamera(all.slice(0, 12), {
      loadFrame: loader(),
      known: new Map(),
      now: 1,
    });
    // Fewer than three neighbours after them, and later pictures may come.
    for (const n of [9, 10, 11]) {
      expect(first.results.get(`CAM/${n}`)).toMatchObject({ early: true, pool: 12 });
    }
    for (const n of [0, 5, 8]) expect(first.results.get(`CAM/${n}`)?.early).toBeUndefined();
    // Nothing new: nothing to do.
    const same = await analyseCamera(all.slice(0, 12), {
      loadFrame: loader(),
      known: first.results,
      now: 2,
    });
    expect(same.results.size).toBe(0);

    const known = new Map(first.results);
    known.set('CAM/10', { ...(known.get('CAM/10') as Detected), corrected: 'animal' });
    const later = await analyseCamera(all, { loadFrame: loader(), known, now: 3 });
    expect(later.results.get('CAM/9')).toMatchObject({ unsure: false, at: 3 });
    expect(later.results.get('CAM/9')?.early).toBeUndefined();
    // The user's word stays.
    expect(later.results.has('CAM/10')).toBe(false);
    expect(later.results.get('CAM/11')?.early).toBeUndefined();
    expect(later.results.get('CAM/19')?.early).toBe(true);
  });

  it('is not early when the pictures after it cannot be neighbours', async () => {
    // Day pictures, then more than eight night pictures: the day picture's
    // candidates after it are full, none of them fits its brightness.
    const list = recordings(20);
    const loadFrame = vi.fn(async (url: string) => {
      const n = Number(url.split('-')[2]);
      const day = grey(n);
      return n < 10 ? day : { ...day, gray: day.gray.map((v) => v - 80), mean: day.mean - 80 };
    });
    const { results } = await analyseCamera(list, { loadFrame, known: new Map(), now: 1 });
    expect(results.get('CAM/9')?.early).toBeUndefined();
    expect(results.get('CAM/19')?.early).toBe(true);
  });

  it('does not read a picture again that it read in a cancelled run', async () => {
    const frames = new Map<string, Frame>();
    let cancel = false;
    const first = vi.fn(async (url: string) => {
      if (url === 'thumb-CAM-5') cancel = true;
      return grey(Number(url.split('-')[2]));
    });
    const cancelled = await analyseCamera(recordings(12), {
      loadFrame: first,
      known: new Map(),
      now: 1,
      frames,
      concurrency: 1,
      isCancelled: () => cancel,
    });
    expect(cancelled.results.size).toBe(0);
    expect(frames.size).toBe(6);
    const second = loader();
    const { results } = await analyseCamera(recordings(12), {
      loadFrame: second,
      known: new Map(),
      now: 2,
      frames,
    });
    expect(results.size).toBe(12);
    expect(second).toHaveBeenCalledTimes(6);
  });

  it('stops when cancelled', async () => {
    const { results } = await analyseCamera(recordings(12), {
      loadFrame: loader(),
      known: new Map(),
      now: 1,
      isCancelled: () => true,
    });
    expect(results.size).toBe(0);
  });

  it('reports progress', async () => {
    const seen: string[] = [];
    await analyseCamera(recordings(12), {
      loadFrame: loader(),
      known: new Map(),
      now: 1,
      concurrency: 1,
      onProgress: (done, total) => seen.push(`${done}/${total}`),
    });
    expect(seen[0]).toBe('1/12');
    expect(seen[seen.length - 1]).toBe('12/12');
  });
});

describe('needsAnalysis', () => {
  const base: Detected = { score: 0, unsure: false, method: METHOD, at: 1_000 };

  it('makes new and outdated results', () => {
    expect(needsAnalysis(undefined, 5, 2_000)).toBe(true);
    expect(needsAnalysis(base, 5, 2_000)).toBe(false);
    expect(needsAnalysis({ ...base, method: 'diff-v1' }, 5, 2_000)).toBe(true);
    expect(needsAnalysis({ ...base, method: 'diff-v1', corrected: 'empty' }, 5, 2_000)).toBe(false);
  });

  it('judges an early result again only when more pictures have come (#458)', () => {
    const early: Detected = { ...base, early: true, pool: 5 };
    expect(needsAnalysis(early, 5, 2_000)).toBe(false);
    expect(needsAnalysis(early, 6, 2_000)).toBe(true);
    expect(needsAnalysis({ ...early, corrected: 'empty' }, 6, 2_000)).toBe(false);
  });

  it('tries again only where that can change something', () => {
    const few: Detected = { ...base, unsure: true, reason: 'few', pool: 5 };
    expect(needsAnalysis(few, 5, 2_000)).toBe(false);
    expect(needsAnalysis(few, 6, 2_000)).toBe(true);
    expect(needsAnalysis({ ...base, unsure: true }, 5, 2_000)).toBe(true);
    expect(needsAnalysis({ ...base, unsure: true, reason: 'scene' }, 50, 9_999_999)).toBe(false);
    const unreadable: Detected = { ...base, unsure: true, reason: 'unreadable' };
    expect(needsAnalysis(unreadable, 5, 1_000 + RETRY_UNREADABLE_S - 1)).toBe(false);
    expect(needsAnalysis(unreadable, 5, 1_000 + RETRY_UNREADABLE_S)).toBe(true);
  });
});

describe('verdictOf and the store', () => {
  const entry: Detected = { score: 0.02, unsure: false, method: METHOD, at: 1 };

  it('depends on the sensitivity: more careful marks fewer as empty', () => {
    expect(verdictOf(entry, 'careful')).toBe('animal');
    expect(verdictOf(entry, 'normal')).toBe('animal');
    expect(verdictOf({ ...entry, score: 0.008 }, 'normal')).toBe('empty');
    expect(verdictOf({ ...entry, score: 0.008 }, 'careful')).toBe('animal');
    expect(verdictOf(undefined, 'normal')).toBe('unsure');
  });

  it('lets the user have the last word', () => {
    expect(verdictOf({ ...entry, corrected: 'empty' }, 'careful')).toBe('empty');
    expect(verdictOf({ ...entry, score: 0, corrected: 'animal' }, 'strict')).toBe('animal');
  });

  it('keeps entries in memory', async () => {
    const store = memoryEmptyStore();
    await store.put('CAM/1', entry);
    entry.at = 99;
    expect((await store.all()).get('CAM/1')?.at).toBe(1);
  });
});
