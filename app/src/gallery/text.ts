import type { Fault, GalleryCamera, RecordingState } from './types';

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

/** A duration as "45 min", "5 h" or "3 Tage". */
export function durationText(seconds: number): string {
  if (seconds < 90 * 60) return `${Math.max(1, Math.round(seconds / 60))} min`;
  if (seconds < 48 * 3600) return `${Math.round(seconds / 3600)} h`;
  return `${Math.round(seconds / 86400)} Tage`;
}

/** A fault for the admin (#265) in German; `camera` is the camera's name. */
export function faultText(fault: Fault, camera: string | undefined, nowSeconds: number): string {
  const percent = fault.value !== undefined ? ` (${fault.value} %)` : '';
  const who = `Kamera „${camera ?? fault.camId ?? '?'}“`;
  switch (fault.kind) {
    case 'hubNeverSeen':
      return 'Der Hub hat sich noch nie gemeldet.';
    case 'hubSilent':
      return (
        `Der Hub meldet sich nicht: ${durationText(fault.value ?? 0)} überfällig` +
        (fault.since ? `, letzter Kontakt ${clockTime(fault.since, nowSeconds)}.` : '.')
      );
    case 'hubBatteryCritical':
      return `Hub-Akku kritisch${percent}.`;
    case 'hubBatteryLow':
      return `Hub-Akku schwach${percent}.`;
    case 'cameraBatteryCritical':
      return `${who}: Akku kritisch${percent}.`;
    case 'cameraBatteryLow':
      return `${who}: Akku schwach${percent}.`;
    case 'sdMissing':
      return `${who}: keine SD-Karte${fault.detail ? ` (${fault.detail})` : ''}.`;
    case 'sdFull':
      return `${who}: SD-Karte voll (${fault.value ?? 0} MB frei).`;
    case 'sdNearlyFull':
      return `${who}: SD-Karte fast voll (${fault.value ?? 0} MB frei).`;
    case 'cameraNotRead':
      return (
        `${who}: seit ${durationText(fault.value ?? 0)} nicht mehr gelesen` +
        (fault.since ? ` (zuletzt ${clockTime(fault.since, nowSeconds)})` : '') +
        ' – außer Reichweite, Akku leer oder ausgeschaltet?'
      );
    case 'firmwareDowngrade':
      return (
        `Der Hub läuft wieder mit älterer Firmware (${fault.detail ?? '?'})` +
        (fault.since ? `, seit ${clockTime(fault.since, nowSeconds)}` : '') +
        ' – Update zurückgerollt?'
      );
    case 'requestsFailed': {
      const n = fault.value ?? 1;
      return (
        `${who}: ${n === 1 ? '1 Auftrag' : `${n} Aufträge`} fehlgeschlagen` +
        (fault.since ? `, zuletzt ${clockTime(fault.since, nowSeconds)}` : '') +
        (fault.detail ? ` – ${fault.detail}` : '') +
        '.'
      );
    }
  }
}

/**
 * What the end user should know about a camera (#304), in plain words; the
 * thresholds are those of the admin's faults (#265).
 */
export function cameraHints(camera: GalleryCamera, hubLastContact: number): string[] {
  const hints: string[] = [];
  const bat = camera.batPercent;
  const read = (camera.lastRead ?? 0) > 0;
  if (read && bat !== undefined && bat <= 10) {
    hints.push(`Der Akku der Kamera ist fast leer (${bat} %). Bitte bald laden oder tauschen.`);
  } else if (read && bat !== undefined && bat <= 25) {
    hints.push(`Der Akku der Kamera wird schwach (${bat} %).`);
  }
  if (camera.sdCard === 'missing') {
    hints.push('In der Kamera steckt keine SD-Karte, oder sie ist defekt.');
  } else if (camera.sdCard === 'ok' && camera.sdFreeMB !== undefined) {
    const free = camera.sdFreeMB;
    const total = camera.sdTotalMB;
    if (free < 200) {
      hints.push('Die SD-Karte der Kamera ist voll. Bitte alte Aufnahmen löschen.');
    } else if (free < 1024 || (total !== undefined && free < total * 0.05)) {
      hints.push('Die SD-Karte der Kamera ist fast voll. Bitte alte Aufnahmen löschen.');
    }
  }
  if (read && hubLastContact - (camera.lastRead ?? 0) > 24 * 3600) {
    hints.push('Die Kamera wurde seit über einem Tag nicht gelesen. Bitte die Kamera prüfen.');
  }
  return hints;
}
