import { describe, expect, it } from 'vitest';
import { handleRequest } from './api';
import type { ApiResponse } from './http';
import type { Camera, CameraReport, CheckinResponse } from './model';
import { checkSettingsChange, parseCapability, parseSettingsReport } from './settings';
import { appRequest, CAM_ID, hubRequest, NOW, setup, USER_ID, userToken } from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

/** A camera report as the hub sends it (#368), shortened. */
const report: CameraReport = {
  camId: CAM_ID,
  readAt: NOW - 60,
  settings: {
    workMode: 0,
    shootMode: 2,
    picResolution: 6,
    pirSens: 2,
    pirCoolingTime: 20,
    osdInfoEnable: true,
    languageCode: 'en',
    micVol: 90,
  },
  capability: {
    imgResolutions: [1, 2, 6, 10],
    videoResolutions: [1, 2, 4],
    languages: ['en', 'de'],
  },
};

async function world() {
  const w = await setup();
  const checkin = (data: unknown = { cameras: [report] }) =>
    handleRequest(hubRequest('/v1/device/checkin', w.hubKey, data), w.deps);
  const ack = (commands: unknown[]) =>
    handleRequest(
      hubRequest('/v1/device/ack', w.hubKey, { cameras: [{ camId: CAM_ID, commands }] }),
      w.deps,
    );
  const call = (method: string, path: string, data?: unknown) =>
    handleRequest(appRequest(method, path, data), w.deps);
  const camera = async () => body<Camera[]>(await call('GET', '/v1/cameras'))[0]!;
  const change = (settings: unknown) =>
    call('POST', `/v1/cameras/${CAM_ID}/commands`, { type: 'setSettings', settings });
  await checkin();
  return { ...w, checkin, ack, call, camera, change };
}

describe('settings of a check-in (#368)', () => {
  it('keeps known fields with plausible values only', () => {
    expect(
      parseSettingsReport({
        workMode: 1,
        pirSens: 7,
        osdInfoEnable: 1,
        secureCode0: 4,
        utcTime: 1,
        languageCode: 'de',
        picResolution: 2.5,
      }),
    ).toEqual({ workMode: 1, languageCode: 'de' });
    expect(parseSettingsReport({ secureCode0: 4 })).toBeUndefined();
    expect(parseSettingsReport(undefined)).toBeUndefined();
    expect(parseSettingsReport([1])).toBeUndefined();
  });

  it('reads the capability lists', () => {
    expect(
      parseCapability({
        imgResolutions: [1, 1, 'x', 200],
        videoResolutions: 3,
        languages: ['en', 5],
      }),
    ).toEqual({ imgResolutions: [1], videoResolutions: [], languages: ['en'] });
    expect(parseCapability(null)).toBeUndefined();
  });

  it('checks a change against the fields and the capability', () => {
    const settings = parseSettingsReport(report.settings);
    const capability = parseCapability(report.capability);
    const check = (change: unknown) => checkSettingsChange(change, settings, capability);
    expect(check({ workMode: 1, osdInfoEnable: false })).toEqual({
      ok: true,
      settings: { workMode: 1, osdInfoEnable: false },
    });
    expect(check({})).toMatchObject({ ok: false });
    expect(check([])).toMatchObject({ ok: false });
    expect(check({ secureCode0: 1 })).toEqual({
      ok: false,
      error: 'secureCode0 cannot be changed',
    });
    expect(check({ workMode: 3 })).toEqual({ ok: false, error: 'Invalid value for workMode' });
    expect(check({ osdInfoEnable: 1 })).toMatchObject({ ok: false });
    expect(check({ picResolution: 3 })).toEqual({
      ok: false,
      error: 'The camera does not offer picResolution 3',
    });
    expect(check({ languageCode: 'fr' })).toMatchObject({ ok: false });
    // Not reported by the hub: the camera does not support it.
    expect(check({ spkVol: 10 })).toEqual({
      ok: false,
      error: 'The camera does not support spkVol',
    });
    // Before the first read only the ranges count.
    expect(checkSettingsChange({ spkVol: 10 }, undefined, undefined)).toMatchObject({ ok: true });
  });
});

describe('changing settings from the gallery (#368)', () => {
  it('shows the settings, hands the change to the hub and takes its answer', async () => {
    const { change, camera, checkin, ack } = await world();
    expect(await camera()).toMatchObject({
      settings: report.settings,
      capability: report.capability,
    });

    const created = await change({ pirSens: 1, osdInfoEnable: false });
    expect(created.statusCode).toBe(201);
    // A second change before the hub has it joins the first.
    const merged = await change({ pirSens: 0, languageCode: 'de' });
    expect(merged.statusCode).toBe(200);
    expect(await camera()).toMatchObject({
      settingsPending: { pirSens: 0, osdInfoEnable: false, languageCode: 'de' },
      settingsState: 'requested',
    });

    const delivered = body<CheckinResponse>(await checkin({ cameras: [] }));
    const commands = delivered.cameras[0]?.commands ?? [];
    expect(commands).toEqual([
      {
        id: 'cmd-1',
        type: 'setSettings',
        settings: { pirSens: 0, osdInfoEnable: false, languageCode: 'de' },
      },
    ]);
    expect((await camera()).settingsState).toBe('delivered');

    // A change while the hub has the first one follows it.
    expect((await change({ micVol: 50 })).statusCode).toBe(201);
    expect((await camera()).settingsPending).toEqual({
      pirSens: 0,
      osdInfoEnable: false,
      languageCode: 'de',
      micVol: 50,
    });

    await ack([{ id: 'cmd-1', state: 'done' }]);
    const after = await camera();
    expect(after.settings).toMatchObject({ pirSens: 0, osdInfoEnable: false, languageCode: 'de' });
    expect(after.settingsPending).toEqual({ micVol: 50 });

    await checkin({ cameras: [] });
    await ack([{ id: 'cmd-2', state: 'failed', error: 'the camera does not support micVol' }]);
    const failed = await camera();
    expect(failed.settingsPending).toBeUndefined();
    expect(failed.settingsError).toBe('the camera does not support micVol');
  });

  it('refuses what the camera cannot take', async () => {
    const { change } = await world();
    expect((await change({ picResolution: 3 })).statusCode).toBe(400);
    expect((await change({ secureCode0: 1 })).statusCode).toBe(400);
    expect((await change({ utcTime: NOW })).statusCode).toBe(400);
    expect((await change(undefined)).statusCode).toBe(400);
  });

  it('takes back a change the hub has not received', async () => {
    const { change, call, camera, checkin } = await world();
    await change({ pirSens: 1 });
    const withdrawn = await call('POST', `/v1/cameras/${CAM_ID}/settings/withdraw`);
    expect(body(withdrawn)).toEqual({ withdrawn: 1 });
    expect((await camera()).settingsPending).toBeUndefined();
    expect(body<CheckinResponse>(await checkin({ cameras: [] })).cameras).toEqual([]);

    await change({ pirSens: 1 });
    await checkin({ cameras: [] });
    expect(body(await call('POST', `/v1/cameras/${CAM_ID}/settings/withdraw`))).toEqual({
      withdrawn: 0,
    });
    expect((await camera()).settingsState).toBe('delivered');
  });

  it('keeps the stored settings when a hub reports none', async () => {
    const { checkin, camera } = await world();
    await checkin({ cameras: [{ camId: CAM_ID, readAt: NOW }] });
    expect((await camera()).settings).toEqual(report.settings);
  });

  it('does not let another user change the camera', async () => {
    const w = await world();
    const res = await handleRequest(
      appRequest(
        'POST',
        `/v1/cameras/${CAM_ID}/commands`,
        { type: 'setSettings', settings: { pirSens: 1 } },
        // Not linked to the hub (test-utils).
        userToken(USER_ID + 1),
      ),
      w.deps,
    );
    expect(res.statusCode).toBe(404);
  });
});
