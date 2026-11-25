import { frameFromRgba, GRID_H, GRID_W, type Frame } from './emptiness';

/**
 * The browser part of the empty-trigger detection (#442): a thumbnail is
 * fetched (the bucket allows the app's origin), drawn onto a small canvas and
 * reduced to a grey frame. `undefined` if anything fails; the recording then
 * has no verdict.
 */
export async function loadFrame(
  url: string,
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<Frame | undefined> {
  try {
    if (typeof createImageBitmap !== 'function') return undefined;
    // Not from the cache: the <img> of the tile loads the same URL without
    // CORS, and a browser may hand that cached answer (without the CORS
    // header) to this request, which then fails.
    const response = await fetchFn(url, { cache: 'no-store' });
    if (!response.ok) return undefined;
    const bitmap = await createImageBitmap(await response.blob());
    try {
      const canvas = document.createElement('canvas');
      canvas.width = GRID_W;
      canvas.height = GRID_H;
      const context = canvas.getContext('2d', { willReadFrequently: true });
      if (!context) return undefined;
      // Shrinking about four times: smooth, so single pixels do not alias into noise.
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = 'high';
      context.drawImage(bitmap, 0, 0, GRID_W, GRID_H);
      return frameFromRgba(context.getImageData(0, 0, GRID_W, GRID_H).data);
    } finally {
      bitmap.close();
    }
  } catch {
    return undefined;
  }
}
