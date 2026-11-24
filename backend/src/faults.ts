import type { CameraRecord, CommandRecord, Fault, FirmwareChange, HubRecord } from './model';

/**
 * Faults of a hub and its cameras for the admin (#265). The end user should
 * not have to interpret them; the admin view lists them. Pure, so the rules
 * are tested without a store.
 */

/** A hub is "silent" this long after the check-in it planned. */
export const SILENT_GRACE_S = 30 * 60;
/** Without a planned check-in, a hub should report at least once a day. */
const DEFAULT_EXPECTED_S = 24 * 60 * 60;
/** A camera not read for this long while its hub checks in is unreachable. */
export const NOT_READ_S = 24 * 60 * 60;
/** Failed requests older than this are no news any more. */
export const FAILED_WINDOW_S = 7 * 24 * 60 * 60;

const HUB_BATTERY_CRITICAL = 10;
const HUB_BATTERY_LOW = 25;
const CAMERA_BATTERY_CRITICAL = 10;
const CAMERA_BATTERY_LOW = 25;
const SD_FULL_MB = 200;
/** Free internal heap below which a check-in may fail (TLS needs ~6 KB, #244). */
export const HUB_HEAP_LOW = 10 * 1024;
const SD_LOW_MB = 1024;
const SD_LOW_SHARE = 0.05;

/**
 * Compares two firmware versions such as "v0.1.13" (git tags; a suffix like
 * "-3-gabc" is ignored). Undefined if one of them has no x.y.z.
 */
export function compareVersions(a: string, b: string): number | undefined {
  const parse = (v: string) => /(\d+)\.(\d+)\.(\d+)/.exec(v)?.slice(1, 4).map(Number);
  const x = parse(a);
  const y = parse(b);
  if (!x || !y) return undefined;
  for (let i = 0; i < 3; i++) {
    const d = (x[i] as number) - (y[i] as number);
    if (d !== 0) return d;
  }
  return 0;
}

/** When the hub should have checked in last, at the latest. */
function expectedCheckin(hub: HubRecord): number {
  const s = hub.schedule;
  const planned = s?.nextCheckinAt ?? s?.nextCameraReadAt;
  if (planned !== undefined) return planned;
  const interval = s && s.checkinIntervalS > 0 ? s.checkinIntervalS : DEFAULT_EXPECTED_S;
  return hub.lastSeen + interval;
}

export function hubFaults(
  hub: HubRecord,
  cameras: Array<{ camera: CameraRecord; commands: CommandRecord[] }>,
  now: number,
  firmwareChanges: FirmwareChange[] = [],
  /** Cameras the user stopped reading (#297): not reading them is no fault. */
  excluded: ReadonlySet<string> = new Set(),
): Fault[] {
  const faults: Fault[] = [];

  // A held-back release (`skipped`) changes nothing on the hub and must not
  // hide the fault of an update before it.
  const changes = firmwareChanges.filter((c) => c.result !== 'skipped');

  // A step back to an older version: the bootloader rolled an update back,
  // or someone flashed an old image (#149).
  const lastChange = [...changes].sort((a, b) => b.at - a.at)[0];
  if (
    lastChange &&
    lastChange.at >= now - FAILED_WINDOW_S &&
    (compareVersions(lastChange.to, lastChange.from) ?? 0) < 0
  ) {
    faults.push({
      kind: 'firmwareDowngrade',
      level: 'warning',
      detail: `${lastChange.from} → ${lastChange.to}`,
      since: lastChange.at,
    });
  }

  // Little free heap in the last cycle: TLS and large responses fail
  // first (#244).
  if (hub.memory && hub.memory.heapMin < HUB_HEAP_LOW) {
    faults.push({
      kind: 'hubMemoryLow',
      level: 'warning',
      value: hub.memory.heapMin,
    });
  }

  // A cell is reporting while the hub runs in mains mode: no deep sleep, the
  // battery drains within hours (#416).
  if (hub.powerMode === 'mains' && hub.batPercent !== undefined) {
    faults.push({ kind: 'powerModeMismatch', level: 'warning' });
  }

  // The hub's own report of a failed or rolled-back update (#149).
  const lastReport = [...changes]
    .filter((c) => c.result !== undefined)
    .sort((a, b) => b.at - a.at)[0];
  if (
    lastReport &&
    (lastReport.result === 'failed' || lastReport.result === 'rolledBack') &&
    lastReport.at >= now - FAILED_WINDOW_S
  ) {
    faults.push({
      kind: 'firmwareUpdateFailed',
      level: 'warning',
      value: lastReport.result === 'rolledBack' ? 1 : 0,
      detail:
        `${lastReport.from} → ${lastReport.to}` +
        (lastReport.reason ? `: ${lastReport.reason}` : ''),
      since: lastReport.at,
    });
  }

  if (hub.lastSeen <= 0) {
    faults.push({ kind: 'hubNeverSeen', level: 'warning' });
  } else {
    const overdue = now - expectedCheckin(hub);
    if (overdue > SILENT_GRACE_S) {
      faults.push({ kind: 'hubSilent', level: 'error', value: overdue, since: hub.lastSeen });
    }
  }

  const stretch = hub.schedule?.stretch;
  const bat = hub.batPercent;
  if (stretch === 'hubBatteryCritical' || (bat !== undefined && bat <= HUB_BATTERY_CRITICAL)) {
    faults.push({ kind: 'hubBatteryCritical', level: 'error', value: bat });
  } else if (stretch === 'hubBatteryLow' || (bat !== undefined && bat <= HUB_BATTERY_LOW)) {
    faults.push({ kind: 'hubBatteryLow', level: 'warning', value: bat });
  }

  // Reading is planned at all: "off" means the hub only reads on request.
  const reads = hub.schedule?.cameraPlan !== 'off';

  for (const { camera, commands } of cameras) {
    const camId = camera.camId;
    // The hub keeps checking in, but this camera has not been read for a day
    // (out of range, battery empty, switched off).
    if (
      reads &&
      !excluded.has(camId) &&
      camera.lastSeen > 0 &&
      hub.lastSeen - camera.lastSeen > NOT_READ_S
    ) {
      faults.push({
        kind: 'cameraNotRead',
        level: 'warning',
        camId,
        value: hub.lastSeen - camera.lastSeen,
        since: camera.lastSeen,
      });
    }
    const cbat = camera.batPercent;
    // 0 is what a camera reports before its first read; only a real read counts.
    if (camera.lastSeen > 0 && cbat !== undefined) {
      if (cbat <= CAMERA_BATTERY_CRITICAL) {
        faults.push({ kind: 'cameraBatteryCritical', level: 'error', camId, value: cbat });
      } else if (cbat <= CAMERA_BATTERY_LOW) {
        faults.push({ kind: 'cameraBatteryLow', level: 'warning', camId, value: cbat });
      }
    }

    if (camera.sdCard === 'missing') {
      faults.push({ kind: 'sdMissing', level: 'error', camId, detail: camera.sdError });
    } else if (camera.sdCard === 'ok' && camera.sdFreeMB !== undefined) {
      const free = camera.sdFreeMB;
      const total = camera.sdTotalMB;
      if (free < SD_FULL_MB) {
        faults.push({ kind: 'sdFull', level: 'error', camId, value: free });
      } else if (free < SD_LOW_MB || (total !== undefined && free < total * SD_LOW_SHARE)) {
        faults.push({ kind: 'sdNearlyFull', level: 'warning', camId, value: free });
      }
    }

    const failed = commands
      .filter(
        (c) =>
          (c.type === 'requestVideo' || c.type === 'deleteMedia') &&
          c.state === 'failed' &&
          c.updated >= now - FAILED_WINDOW_S,
      )
      .sort((a, b) => b.updated - a.updated);
    const latest = failed[0];
    if (latest) {
      faults.push({
        kind: 'requestsFailed',
        level: 'warning',
        camId,
        value: failed.length,
        detail: latest.error,
        since: latest.updated,
      });
    }
  }
  return faults;
}

/**
 * The faults a user of the hub can do something about (#431): the hub not
 * reporting, batteries, SD cards, a camera not read. The rest (failed
 * requests, firmware, memory, power mode) is for the admin.
 */
const USER_KINDS: ReadonlySet<Fault['kind']> = new Set<Fault['kind']>([
  'hubNeverSeen',
  'hubSilent',
  'hubBatteryCritical',
  'hubBatteryLow',
  'cameraBatteryCritical',
  'cameraBatteryLow',
  'sdMissing',
  'sdFull',
  'sdNearlyFull',
  'cameraNotRead',
]);

export function userFaults(faults: Fault[]): Fault[] {
  return faults.filter((f) => USER_KINDS.has(f.kind));
}
