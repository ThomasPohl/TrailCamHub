import { describe, expect, it } from 'vitest';
import type { MediaItem } from '../api/types';
import {
  compareMasks,
  judge,
  maskFromRgba,
  MIN_TEXT_PIXELS,
  splitMismatchedPairs,
  type StampMask,
} from './stamp';

/** A mask with the given text pixels (indices) set. */
function mask(on: number[], width = 20, height = 10): StampMask {
  const text = new Uint8Array(width * height);
  for (const i of on) text[i] = 1;
  return { width, height, text };
}

const range = (from: number, to: number) => Array.from({ length: to - from }, (_, i) => from + i);

describe('date and time line of a thumbnail (#347)', () => {
  it('turns bright pixels into text', () => {
    const rgba = Uint8ClampedArray.from([255, 255, 255, 255, 40, 40, 40, 255, 171, 171, 171, 255]);
    expect(Array.from(maskFromRgba(rgba, 3, 1).text)).toEqual([1, 0, 1]);
  });

  it('counts the text pixels that differ', () => {
    const result = compareMasks(mask(range(0, 100)), mask(range(10, 110)));
    expect(result).toEqual({ different: 20, text: 110, ratio: 20 / 110 });
  });

  it('cannot compare lines of another size', () => {
    expect(compareMasks(mask([1], 20, 10), mask([1], 10, 20))).toBeUndefined();
  });

  it('calls the same line the same moment, even with a little noise', () => {
    const line = range(0, 200);
    expect(judge(compareMasks(mask(line), mask(line)))).toBe(true);
    expect(judge(compareMasks(mask(line), mask([...line.slice(3), 300])))).toBe(true);
  });

  it('calls a line with other digits another moment', () => {
    // 44 and 45 of the test camera: 39 of 328 text pixels differed.
    expect(judge({ different: 39, text: 328, ratio: 39 / 328 })).toBe(false);
  });

  it('does not judge without text', () => {
    expect(judge(compareMasks(mask([]), mask([])))).toBeUndefined();
    expect(judge(compareMasks(mask(range(0, MIN_TEXT_PIXELS - 1)), mask([])))).toBeUndefined();
    expect(judge(undefined)).toBeUndefined();
  });
});

function item(num: number, fileType: 0 | 2): MediaItem {
  return {
    mediaId: num,
    dir: 100,
    num,
    fileType,
    mediaTime: 1_000 + num,
    thumbUrl: `https://thumb/${num}`,
    videoState: 'none',
  } as MediaItem;
}

describe('splitting pairs by date and time (#347)', () => {
  const pair = [item(43, 0), item(42, 2)];
  const other = [item(44, 0), item(45, 2)];
  const alone = [item(41, 0)];
  const nums = (groups: MediaItem[][]) => groups.map((g) => g.map((m) => m.num));

  it('splits a pair whose lines differ, video first, and keeps the others', async () => {
    const groups = await splitMismatchedPairs(
      [alone, other, pair],
      async (video) => video.num === 42,
    );
    expect(nums(groups)).toEqual([[41], [45], [44], [43, 42]]);
  });

  it('keeps a pair when there is no answer', async () => {
    const groups = await splitMismatchedPairs([pair], async () => undefined);
    expect(nums(groups)).toEqual([[43, 42]]);
  });

  it('keeps a pair whose check is too slow', async () => {
    const never = () => new Promise<boolean>(() => undefined);
    const groups = await splitMismatchedPairs([pair], never, 20);
    expect(nums(groups)).toEqual([[43, 42]]);
  });

  it('checks no single files', async () => {
    let calls = 0;
    await splitMismatchedPairs([alone], async () => {
      calls++;
      return false;
    });
    expect(calls).toBe(0);
  });
});
