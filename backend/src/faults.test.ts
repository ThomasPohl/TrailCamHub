import { describe, expect, it } from 'vitest';
import { compareVersions, FAILED_WINDOW_S, hubFaults, SILENT_GRACE_S } from './faults';
import type { CameraRecord, CommandRecord, HubRecord } from './model';

const NOW = 1_800_000_000;

const hub = (over: Partial<HubRecord> = {}): HubRecord => ({
  hubId: 'h1',
  name: 'Garten',
  keyHash: '',
  lastSeen: NOW - 600,
  batPercent: 80,
  schedule: {
    cameraPlan: '*=120',
    cameraPlanOrigin: 'local',
    checkinIntervalS: 3600,
    checkinOrigin: 'local',
    nextCheckinAt: NOW + 3000,
    sleeps: true,
  },
  ...over,
});

const camera = (over: Partial<CameraRecord> = {}): CameraRecord => ({
  camId: 'C1',
  hubId: 'h1',
  name: 'Wiese',
  batPercent: 70,
  sdCard: 'ok',
  sdFreeMB: 20_000,
  sdTotalMB: 30_000,
  lastSeen: NOW - 900,
  ...over,
});

const failed = (updated: number, error = 'too large'): CommandRecord => ({
  id: `c${updated}`,
  type: 'requestVideo',
  mediaId: 1,
  state: 'failed',
  created: updated - 60,
  updated,
  error,
});

const kinds = (faults: ReturnType<typeof hubFaults>) => faults.map((f) => f.kind);

describe('hub faults (#265)', () => {
  it('finds nothing on a healthy hub', () => {
    expect(hubFaults(hub(), [{ camera: camera(), commands: [] }], NOW)).toEqual([]);
  });

  it('notices a hub that missed its planned check-in', () => {
    const planned = NOW - SILENT_GRACE_S - 60;
    const silent = hub({
      lastSeen: planned - 3600,
      schedule: { ...hub().schedule!, nextCheckinAt: planned },
    });
    expect(hubFaults(silent, [], NOW)).toEqual([
      { kind: 'hubSilent', level: 'error', value: SILENT_GRACE_S + 60, since: planned - 3600 },
    ]);
    // Within the grace time it is no fault yet.
    const late = hub({ schedule: { ...hub().schedule!, nextCheckinAt: NOW - 60 } });
    expect(hubFaults(late, [], NOW)).toEqual([]);
    expect(kinds(hubFaults(hub({ lastSeen: 0 }), [], NOW))).toEqual(['hubNeverSeen']);
  });

  it('falls back to a day without a planned check-in', () => {
    const noPlan = hub({ schedule: undefined, lastSeen: NOW - 24 * 3600 - SILENT_GRACE_S - 1 });
    expect(kinds(hubFaults(noPlan, [], NOW))).toEqual(['hubSilent']);
  });

  it('reports low batteries of hub and cameras', () => {
    expect(kinds(hubFaults(hub({ batPercent: 8 }), [], NOW))).toEqual(['hubBatteryCritical']);
    expect(kinds(hubFaults(hub({ batPercent: 20 }), [], NOW))).toEqual(['hubBatteryLow']);
    const stretched = hub({ schedule: { ...hub().schedule!, stretch: 'hubBatteryCritical' } });
    expect(kinds(hubFaults(stretched, [], NOW))).toEqual(['hubBatteryCritical']);

    const cams = [
      { camera: camera({ camId: 'A', batPercent: 5 }), commands: [] },
      { camera: camera({ camId: 'B', batPercent: 22 }), commands: [] },
      // Never read: its 0 % is no measurement.
      { camera: camera({ camId: 'C', batPercent: 0, lastSeen: 0 }), commands: [] },
    ];
    const faults = hubFaults(hub(), cams, NOW);
    expect(faults.map((f) => [f.kind, f.camId, f.value])).toEqual([
      ['cameraBatteryCritical', 'A', 5],
      ['cameraBatteryLow', 'B', 22],
    ]);
  });

  it('reports SD cards that are missing or full', () => {
    const cams = [
      { camera: camera({ camId: 'A', sdCard: 'missing', sdError: 'no card' }), commands: [] },
      { camera: camera({ camId: 'B', sdFreeMB: 150 }), commands: [] },
      { camera: camera({ camId: 'C', sdFreeMB: 900 }), commands: [] },
      { camera: camera({ camId: 'D', sdFreeMB: 3_000, sdTotalMB: 128_000 }), commands: [] },
    ];
    expect(hubFaults(hub(), cams, NOW).map((f) => [f.kind, f.camId, f.level])).toEqual([
      ['sdMissing', 'A', 'error'],
      ['sdFull', 'B', 'error'],
      ['sdNearlyFull', 'C', 'warning'],
      ['sdNearlyFull', 'D', 'warning'],
    ]);
  });

  it('counts failed requests of the last week, with the latest reason', () => {
    const commands = [
      failed(NOW - 3600, 'too large'),
      failed(NOW - 60, 'no longer on the camera'),
      failed(NOW - FAILED_WINDOW_S - 1),
      { ...failed(NOW - 30), state: 'done' as const },
    ];
    expect(hubFaults(hub(), [{ camera: camera(), commands }], NOW)).toEqual([
      {
        kind: 'requestsFailed',
        level: 'warning',
        camId: 'C1',
        value: 2,
        detail: 'no longer on the camera',
        since: NOW - 60,
      },
    ]);
  });

  it('warns about a step back to an older firmware (#149)', () => {
    expect(compareVersions('v0.1.13', 'v0.1.9')).toBeGreaterThan(0);
    expect(compareVersions('v0.2.0', 'v0.10.0')).toBeLessThan(0);
    expect(compareVersions('v0.1.13-3-gabc', 'v0.1.13')).toBe(0);
    expect(compareVersions('dev', 'v0.1.13')).toBeUndefined();

    const back = { at: NOW - 60, from: 'v0.1.13', to: 'v0.1.12' };
    expect(hubFaults(hub(), [], NOW, [back])).toEqual([
      { kind: 'firmwareDowngrade', level: 'warning', detail: 'v0.1.13 → v0.1.12', since: NOW - 60 },
    ]);
    const forward = { at: NOW - 30, from: 'v0.1.12', to: 'v0.1.13' };
    expect(hubFaults(hub(), [], NOW, [back, forward])).toEqual([]);
    expect(hubFaults(hub(), [], NOW, [{ ...back, at: NOW - FAILED_WINDOW_S - 1 }])).toEqual([]);
  });

  it('notices a camera that is no longer read (#297)', () => {
    const stale = camera({ lastSeen: NOW - 600 - 2 * 86_400 });
    expect(hubFaults(hub(), [{ camera: stale, commands: [] }], NOW)).toEqual([
      {
        kind: 'cameraNotRead',
        level: 'warning',
        camId: 'C1',
        value: 2 * 86_400,
        since: NOW - 600 - 2 * 86_400,
      },
    ]);
    // Excluded by the user, or a plan that reads only on request: no fault.
    expect(hubFaults(hub(), [{ camera: stale, commands: [] }], NOW, [], new Set(['C1']))).toEqual(
      [],
    );
    const off = hub({ schedule: { ...hub().schedule!, cameraPlan: 'off' } });
    expect(hubFaults(off, [{ camera: stale, commands: [] }], NOW)).toEqual([]);
  });
});
