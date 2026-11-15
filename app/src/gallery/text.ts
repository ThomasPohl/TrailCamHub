import type { RecordingState } from './types';

/**
 * German texts and formatting for the gallery. The end user reads German;
 * code and comments stay English.
 */

const pad = (n: number) => String(n).padStart(2, '0');

/** Camera wall clock (docs/clock.md) as "15.11. 14:02". */
export function cameraTime(cameraSeconds: number): string {
  const d = new Date(cameraSeconds * 1000);
  return `${pad(d.getUTCDate())}.${pad(d.getUTCMonth() + 1)}. ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
}

/** A real instant in the browser's zone as "14:02" (today) or "15.11. 14:02". */
export function clockTime(unixSeconds: number, nowSeconds: number): string {
  const d = new Date(unixSeconds * 1000);
  const today = new Date(nowSeconds * 1000).toDateString() === d.toDateString();
  const time = `${pad(d.getHours())}:${pad(d.getMinutes())}`;
  return today ? `${time} Uhr` : `${pad(d.getDate())}.${pad(d.getMonth() + 1)}. ${time} Uhr`;
}

/** "Garten_2026-11-15_14-02_100-36.mp4" from the camera wall clock. */
export function videoFileName(cameraName: string, cameraSeconds: number, num: number): string {
  const d = new Date(cameraSeconds * 1000);
  const safe = cameraName.replace(/[^A-Za-z0-9ÄÖÜäöüß_-]+/g, '-') || 'Kamera';
  const date = `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}`;
  const time = `${pad(d.getUTCHours())}-${pad(d.getUTCMinutes())}`;
  return `${safe}_${date}_${time}_${num}.mp4`;
}

export function stateText(
  state: RecordingState,
  saved: boolean,
  nextContact: number | undefined,
  now: number,
): string {
  switch (state) {
    case 'new':
      return 'Neu';
    case 'videoRequested':
      return nextContact
        ? `Video kommt, voraussichtlich gegen ${clockTime(nextContact, now)}`
        : 'Video kommt beim nächsten Kontakt';
    case 'videoReady':
      return saved ? 'Video ist gespeichert' : 'Video ist da';
    case 'deleteRequested':
      return 'Wird gelöscht';
    case 'deleted':
      return 'Gelöscht';
    case 'failed':
      return 'Hat nicht geklappt';
  }
}
