/**
 * The TC100 prints an info line (moon phase, temperature, date, time) across
 * the bottom of every picture. Two files that belong together (a video and
 * its own photo) should show the same line; a separate photo taken a few
 * seconds earlier shows another time (#347).
 *
 * This only measures: the numbers are logged with `?debug`, so the rule can
 * be set from real thumbnails before it changes any tile.
 */

/** Share of the picture height, from the bottom, that holds the info line. */
export const STRIP_FRACTION = 0.1;
/** Every picture is scaled to this size before it is compared. */
export const COMPARE_WIDTH = 320;
export const COMPARE_HEIGHT = 40;

/** Grey values (0..255), `COMPARE_WIDTH` x `COMPARE_HEIGHT` for the strip. */
export interface Pixels {
  width: number;
  height: number;
  grey: Uint8ClampedArray;
}

/** Mean absolute difference of two grey pictures of the same size, 0..255. */
export function meanDifference(a: Pixels, b: Pixels): number {
  if (a.width !== b.width || a.height !== b.height || a.grey.length !== b.grey.length) {
    throw new Error('Pictures differ in size');
  }
  if (a.grey.length === 0) return 0;
  let sum = 0;
  for (let i = 0; i < a.grey.length; i++) {
    sum += Math.abs((a.grey[i] as number) - (b.grey[i] as number));
  }
  return sum / a.grey.length;
}

/** RGBA pixels to grey values (Rec. 601 weights). */
export function toGrey(rgba: Uint8ClampedArray): Uint8ClampedArray {
  const grey = new Uint8ClampedArray(rgba.length / 4);
  for (let i = 0; i < grey.length; i++) {
    grey[i] =
      0.299 * (rgba[i * 4] as number) +
      0.587 * (rgba[i * 4 + 1] as number) +
      0.114 * (rgba[i * 4 + 2] as number);
  }
  return grey;
}

export interface Measurement {
  /** Difference of the bottom strip: the time stamp. */
  strip: number;
  /** Difference of the whole picture: how alike the scenes are. */
  image: number;
}

/** Downloads a thumbnail and returns its whole picture and bottom strip. */
async function loadPictures(
  url: string,
  fetchFn: typeof fetch,
): Promise<{ strip: Pixels; image: Pixels }> {
  const response = await fetchFn(url);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const bitmap = await createImageBitmap(await response.blob());
  try {
    const draw = (sx: number, sy: number, sw: number, sh: number): Pixels => {
      const canvas = document.createElement('canvas');
      canvas.width = COMPARE_WIDTH;
      canvas.height = COMPARE_HEIGHT;
      const context = canvas.getContext('2d', { willReadFrequently: true });
      if (!context) throw new Error('No canvas');
      context.drawImage(bitmap, sx, sy, sw, sh, 0, 0, COMPARE_WIDTH, COMPARE_HEIGHT);
      const data = context.getImageData(0, 0, COMPARE_WIDTH, COMPARE_HEIGHT).data;
      return { width: COMPARE_WIDTH, height: COMPARE_HEIGHT, grey: toGrey(data) };
    };
    const stripHeight = Math.max(1, Math.round(bitmap.height * STRIP_FRACTION));
    return {
      strip: draw(0, bitmap.height - stripHeight, bitmap.width, stripHeight),
      image: draw(0, 0, bitmap.width, bitmap.height),
    };
  } finally {
    bitmap.close();
  }
}

/** Compares two thumbnails; `undefined` when one cannot be loaded. */
export async function measureThumbnails(
  urlA: string,
  urlB: string,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<Measurement | undefined> {
  try {
    const [a, b] = await Promise.all([loadPictures(urlA, fetchFn), loadPictures(urlB, fetchFn)]);
    return { strip: meanDifference(a.strip, b.strip), image: meanDifference(a.image, b.image) };
  } catch {
    return undefined;
  }
}
