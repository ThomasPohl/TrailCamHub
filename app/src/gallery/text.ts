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

/**
 * The camera's name for a file on its SD card without suffix, e.g.
 * "DSCF0012": DSCF + the 4-digit number of the media list.
 */
export function cameraFileName(num: number): string {
  return `DSCF${String(num).padStart(4, '0')}`;
}

/** The full name of a file on the camera: DSCF0011.JPG (photo), DSCF0012.AVI (video). */
export function cameraFile(num: number, fileType: number): string {
  return `${cameraFileName(num)}.${fileType === 2 ? 'AVI' : 'JPG'}`;
}

/**
 * The sub-folder on the laptop for a camera's videos: its name without the
 * characters Windows does not allow in a folder name (the user names the
 * cameras himself, #273).
 */
export function folderName(cameraName: string): string {
  const safe = cameraName
    .replace(/[<>:"/\\|?*]+/g, '-')
    .replace(/[. ]+$/, '')
    .trim();
  return safe || 'Kamera';
}

/** "Garten_2026-11-15_14-02_DSCF0036.mp4" from the camera wall clock. */
export function videoFileName(cameraName: string, cameraSeconds: number, num: number): string {
  const d = new Date(cameraSeconds * 1000);
  const safe = cameraName.replace(/[^A-Za-z0-9ÄÖÜäöüß_-]+/g, '-') || 'Kamera';
  const date = `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}`;
  const time = `${pad(d.getUTCHours())}-${pad(d.getUTCMinutes())}`;
  return `${safe}_${date}_${time}_${cameraFileName(num)}.mp4`;
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
