import { describe, expect, it, vi } from 'vitest';
import { analyseCamera, memoryEmptyStore, verdictOf, type Detected } from './emptyDetector';
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
    expect([...second.results.keys()].sort()).toEqual(['CAM/12', 'CAM/13']);
    // The two new ones and the eight before them, as neighbours.
    expect(loadFrame).toHaveBeenCalledTimes(10);
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
    expect([...few.results.values()].every((d) => d.unsure)).toBe(true);
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
    expect(results.has('CAM/3')).toBe(false);
    expect(results.size).toBe(9);
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
