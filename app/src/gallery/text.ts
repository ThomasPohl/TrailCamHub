import type { BatteryForecast } from './forecast';
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

/** Consumption and empty-battery forecast (docs: admin view) in German. */
export function forecastText(f: BatteryForecast, nowSeconds: number): string {
  const rate = Math.abs(f.drainPerHour).toFixed(1).replace('.', ',');
  const over = `(letzte ${durationText(f.windowS)})`;
  if (f.emptyAt === undefined) {
    return f.drainPerHour <= -0.05
      ? `Lädt: +${rate} %/h ${over}, keine Prognose.`
      : `Verbrauch: ${rate} %/h ${over}, Akku hält praktisch dauerhaft.`;
  }
  const left = f.emptyAt - nowSeconds;
  const age = nowSeconds - f.lastAt;
  // The forecast starts at the latest value, so an old value moves it into the past.
  const since = age > 3 * 3600 ? ` Letzter Wert von ${clockTime(f.lastAt, nowSeconds)}.` : '';
  const when =
    left <= 0
      ? `Laut Prognose war der Akku ${clockTime(f.emptyAt, nowSeconds)} leer.${since}`
      : left > 365 * 86400
        ? 'Leer: nicht absehbar (mehr als ein Jahr).'
        : `Leer voraussichtlich ${clockTime(f.emptyAt, nowSeconds)} (in ${durationText(left)}).${since}`;
  return `Verbrauch: ${rate} %/h ${over}. ${when}`;
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
    case 'hubMemoryLow':
      return `Der Hub hatte zuletzt wenig freien Speicher (${kilobytes(fault.value ?? 0)}); Kontakte können scheitern.`;
    case 'firmwareUpdateFailed':
      return (
        `Firmware-Update ${fault.value === 1 ? 'zurückgerollt' : 'fehlgeschlagen'} (${fault.detail ?? '?'})` +
        (fault.since ? `, ${clockTime(fault.since, nowSeconds)}` : '')
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

const UPLINK = { wifi: 'WLAN', modem: 'Mobilfunk', ethernet: 'Ethernet' } as const;

/** "Mobilfunk (Telekom.de), Signal -87 dBm (gut)" (#149). */
export function uplinkText(
  uplink: keyof typeof UPLINK,
  signalDbm?: number,
  operator?: string,
): string {
  if (uplink !== 'modem') return UPLINK[uplink];
  const network = operator ? `${UPLINK.modem} (${operator})` : UPLINK.modem;
  if (signalDbm === undefined) return network;
  const quality =
    signalDbm >= -85
      ? 'gut'
      : signalDbm >= -95
        ? 'mittel'
        : signalDbm >= -105
          ? 'schwach'
          : 'sehr schwach';
  return `${network}, Signal ${signalDbm} dBm (${quality})`;
}

/** The hub's reasons for holding back a release (`decision_text()` of the firmware). */
const SKIP_REASON: Record<string, string> = {
  'the hub battery is too low': 'Akku zu schwach',
  'the hub battery is critical': 'Akku kritisch',
  'the cellular signal is too weak': 'Mobilfunksignal zu schwach',
  'this version was rolled back before': 'wurde schon einmal zurückgerollt',
  'its install failed too often; trying again later':
    'Installation schlug zu oft fehl, neuer Versuch später',
};

/** Why the hub did not install a release (#142, #149); unknown reasons as sent. */
export function skipReasonText(reason: string): string {
  return SKIP_REASON[reason] ?? reason;
}

/** Bytes as "6.6 KB" or "3.1 MB". */
export function kilobytes(bytes: number): string {
  return bytes >= 1024 * 1024
    ? `${(bytes / 1024 / 1024).toFixed(1)} MB`
    : `${(bytes / 1024).toFixed(1)} KB`;
}

/** The hub's lowest free memory in one line (#149, #244). */
export function memoryText(m: {
  heapMin: number;
  psramMin?: number;
  stackSession?: number;
  stackUplink?: number;
  stackController?: number;
}): string {
  const parts = [`intern min. ${kilobytes(m.heapMin)} frei`];
  if (m.psramMin !== undefined) parts.push(`PSRAM min. ${kilobytes(m.psramMin)}`);
  const stacks = [
    ['Sitzung', m.stackSession],
    ['Upload', m.stackUplink],
    ['Steuerung', m.stackController],
  ]
    .filter((s): s is [string, number] => s[1] !== undefined)
    .map(([name, bytes]) => `${name} ${kilobytes(bytes)}`);
  if (stacks.length > 0) parts.push(`Stack: ${stacks.join(', ')}`);
  return parts.join(' · ');
}
