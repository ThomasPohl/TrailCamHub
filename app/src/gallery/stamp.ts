/**
 * The TC100 prints an info line across the bottom of every thumbnail: battery,
 * moon phase, temperature, date, time and the camera name, white on a dark bar
 * (about 16 of 176 pixels). A video and its own photo show the same date and
 * time, separate shots show another one, even if the scene looks alike (#347).
 * The scenes themselves differ a lot inside a real pair, so only the white
 * pixels of the date and time are compared, never the picture.
 */
import type { MediaItem } from '../api/types';

/** Share of the picture height, from the bottom, that holds the info bar. */
export const BAR_FRACTION = 0.09;
/** The date and time part of the line, as shares of the picture width. */
export const TIME_FROM = 0.4;
export const TIME_TO = 0.88;
/** Grey value from which a pixel counts as text. */
export const TEXT_GREY = 170;
/** Two lines are the same if at most this share of their text pixels differs. */
export const MAX_MISMATCH = 0.03;
/** With less text than this there is no line to compare. */
export const MIN_TEXT_PIXELS = 50;

/** The text pixels (1) of the cropped info line. */
export interface StampMask {
  width: number;
  height: number;
  text: Uint8Array;
}

export interface Comparison {
  /** Pixels that are text in only one of the two. */
  different: number;
  /** Pixels that are text in at least one of the two. */
  text: number;
  ratio: number;
}

/** RGBA pixels of the cropped line to a mask of the bright text pixels. */
export function maskFromRgba(rgba: Uint8ClampedArray, width: number, height: number): StampMask {
  const text = new Uint8Array(width * height);
  for (let i = 0; i < text.length; i++) {
    const grey =
      0.299 * (rgba[i * 4] as number) +
      0.587 * (rgba[i * 4 + 1] as number) +
      0.114 * (rgba[i * 4 + 2] as number);
    text[i] = grey > TEXT_GREY ? 1 : 0;
  }
  return { width, height, text };
}

/** `undefined` when the two lines cannot be compared (other size). */
export function compareMasks(a: StampMask, b: StampMask): Comparison | undefined {
  if (a.width !== b.width || a.height !== b.height) return undefined;
  let different = 0;
  let text = 0;
  for (let i = 0; i < a.text.length; i++) {
    const x = a.text[i] as number;
    const y = b.text[i] as number;
    if (x !== y) different++;
    if (x || y) text++;
  }
  return { different, text, ratio: text === 0 ? 0 : different / text };
}

/**
 * Do the two lines show the same moment? `undefined` when there is nothing to
 * judge by (other size, no text), so the caller keeps what it knew before.
 */
export function judge(result: Comparison | undefined): boolean | undefined {
  if (!result || result.text < MIN_TEXT_PIXELS) return undefined;
  return result.ratio <= MAX_MISMATCH;
}

/** Downloads a thumbnail and returns its date and time line. */
export async function loadStamp(
  url: string,
  fetchFn: typeof fetch,
): Promise<StampMask | undefined> {
  try {
    if (typeof createImageBitmap !== 'function') return undefined;
    const response = await fetchFn(url);
    if (!response.ok) return undefined;
    const bitmap = await createImageBitmap(await response.blob());
    try {
      const x0 = Math.round(bitmap.width * TIME_FROM);
      const width = Math.round(bitmap.width * TIME_TO) - x0;
      const height = Math.max(8, Math.round(bitmap.height * BAR_FRACTION));
      const canvas = document.createElement('canvas');
      canvas.width = width;
      canvas.height = height;
      const context = canvas.getContext('2d', { willReadFrequently: true });
      if (!context) return undefined;
      context.drawImage(bitmap, x0, bitmap.height - height, width, height, 0, 0, width, height);
      return maskFromRgba(context.getImageData(0, 0, width, height).data, width, height);
    } finally {
      bitmap.close();
    }
  } catch {
    return undefined;
  }
}

/** Compares the date and time line of two thumbnails; `undefined` if one cannot be read. */
export async function compareThumbnails(
  urlA: string,
  urlB: string,
  fetchFn: typeof fetch,
): Promise<Comparison | undefined> {
  const [a, b] = await Promise.all([loadStamp(urlA, fetchFn), loadStamp(urlB, fetchFn)]);
  return a && b ? compareMasks(a, b) : undefined;
}

/**
 * Splits every video/photo group whose two thumbnails show another date and
 * time. A check that has no answer (no text, thumbnail not readable, too slow)
 * keeps the group: then the numbers decide, as before.
 */
export async function splitMismatchedPairs(
  groups: MediaItem[][],
  same: (video: MediaItem, photo: MediaItem) => Promise<boolean | undefined>,
  waitMs = 4_000,
  parallel = 4,
): Promise<MediaItem[][]> {
  const pairs = groups
    .map((files, index) => ({
      index,
      video: files.find((m) => m.fileType === 2),
      photo: files.find((m) => m.fileType !== 2),
    }))
    .filter((p) => p.video && p.photo);
  const verdict = new Map<number, boolean | undefined>();
  let next = 0;
  const worker = async () => {
    while (next < pairs.length) {
      const pair = pairs[next++] as (typeof pairs)[number];
      verdict.set(pair.index, await same(pair.video as MediaItem, pair.photo as MediaItem));
    }
  };
  let timer: ReturnType<typeof setTimeout> | undefined;
  await Promise.race([
    Promise.all(Array.from({ length: Math.min(parallel, pairs.length) }, worker)),
    new Promise<void>((resolve) => {
      timer = setTimeout(resolve, waitMs);
    }),
  ]);
  clearTimeout(timer);
  return groups.flatMap((files, index) => {
    if (verdict.get(index) !== false) return [files];
    const video = files.find((m) => m.fileType === 2) as MediaItem;
    const photo = files.find((m) => m.fileType !== 2) as MediaItem;
    return [[video], [photo]];
  });
}
