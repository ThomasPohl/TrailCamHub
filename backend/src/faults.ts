import type { CameraRecord, CommandRecord, Fault, HubRecord } from './model';

/**
 * Faults of a hub and its cameras for the admin (#265). The end user should
 * not have to interpret them; the admin view lists them. Pure, so the rules
 * are tested without a store.
 */

/** A hub is "silent" this long after the check-in it planned. */
export const SILENT_GRACE_S = 30 * 60;
/** Without a planned check-in, a hub should report at least once a day. */
const DEFAULT_EXPECTED_S = 24 * 60 * 60;
/** Failed requests older than this are no news any more. */
export const FAILED_WINDOW_S = 7 * 24 * 60 * 60;

const HUB_BATTERY_CRITICAL = 10;
const HUB_BATTERY_LOW = 25;
const CAMERA_BATTERY_CRITICAL = 10;
const CAMERA_BATTERY_LOW = 25;
const SD_FULL_MB = 200;
const SD_LOW_MB = 1024;
const SD_LOW_SHARE = 0.05;

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
): Fault[] {
  const faults: Fault[] = [];

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

  for (const { camera, commands } of cameras) {
    const camId = camera.camId;
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
