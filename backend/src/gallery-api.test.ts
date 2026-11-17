import { describe, expect, it } from 'vitest';
import { hubFaults } from './faults';
import { createAppUser, revokeAppUser } from './admin';
import { handleRequest } from './api';
import type { ApiRequest, ApiResponse } from './http';
import type { CheckinRequest, CheckinResponse, MediaItem } from './model';
import { CAM_ID, HUB_ID, hubRequest, NOW, setup } from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

const checkinBody: CheckinRequest = {
  cameras: [
    {
      camId: CAM_ID,
      readAt: NOW - 60,
      media: [
        { mediaId: 2035, dir: 100, num: 35, fileType: 2, mediaTime: NOW - 600 },
        { mediaId: 2037, dir: 100, num: 37, fileType: 2, mediaTime: NOW - 300 },
      ],
    },
  ],
};

function galleryRequest(token: string, method: string, path: string, data?: unknown): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `Bearer ${token}` },
    body: data === undefined ? undefined : JSON.stringify(data),
  };
}

/** One camera with two videos whose thumbnails arrived, and a gallery user. */
async function galleryWorld() {
  const world = await setup();
  await handleRequest(hubRequest('/v1/device/checkin', world.hubKey, checkinBody), world.deps);
  await world.store.markThumbPresent(CAM_ID, 2035);
  await world.store.markThumbPresent(CAM_ID, 2037);
  const user = await createAppUser(
    { admin: 'createAppUser', name: 'Opa', hubIds: [HUB_ID] },
    world.deps,
  );
  if (!user.ok) throw new Error(user.error);
  const call = (method: string, path: string, data?: unknown) =>
    handleRequest(galleryRequest(user.token, method, path, data), world.deps);
  return { ...world, user, call };
}

describe('gallery app users (#259)', () => {
  it('signs in with the app token and sees the cameras of linked hubs', async () => {
    const { call, user } = await galleryWorld();
    expect(String(user.userId)).toHaveLength(16);
    const cameras = await call('GET', '/v1/cameras');
    expect(cameras.statusCode).toBe(200);
    expect(body<Array<{ camId: string }>>(cameras).map((c) => c.camId)).toEqual([CAM_ID]);
  });

  it('rejects a wrong or rotated token', async () => {
    const world = await galleryWorld();
    const bad = `${world.user.userId}.${'x'.repeat(43)}`;
    const res = await handleRequest(galleryRequest(bad, 'GET', '/v1/cameras'), world.deps);
    expect(res.statusCode).toBe(401);

    const rotated = await createAppUser(
      { admin: 'createAppUser', userId: world.user.userId },
      world.deps,
    );
    expect(rotated.ok).toBe(true);
    expect((await world.call('GET', '/v1/cameras')).statusCode).toBe(401);
  });

  it('does not accept a hub key as app token', async () => {
    const world = await galleryWorld();
    const res = await handleRequest(galleryRequest(world.hubKey, 'GET', '/v1/cameras'), world.deps);
    expect(res.statusCode).toBe(401);
  });
});

describe('admin role and locking out (#259)', () => {
  it('shows the admin the cameras of every hub', async () => {
    const world = await galleryWorld();
    await handleRequest(
      hubRequest('/v1/device/checkin', world.otherHubKey, {
        cameras: [{ camId: 'CAM_OTHER', readAt: NOW - 60, media: [] }],
      }),
      world.deps,
    );
    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const asAdmin = (method: string, path: string, data?: unknown) =>
      handleRequest(galleryRequest(admin.token, method, path, data), world.deps);

    const me = body<{ role: string; name: string; hubs: Array<{ hubId: string }> }>(
      await asAdmin('GET', '/v1/me'),
    );
    expect(me).toMatchObject({ role: 'admin', name: 'Thomas' });
    expect(me.hubs.map((h) => h.hubId)).toEqual([HUB_ID, 'other-hub']);
    const ids = body<Array<{ camId: string }>>(await asAdmin('GET', '/v1/cameras')).map(
      (c) => c.camId,
    );
    expect(ids.sort()).toEqual([CAM_ID, 'CAM_OTHER'].sort());
    expect((await asAdmin('GET', `/v1/cameras/${CAM_ID}/media`)).statusCode).toBe(200);

    // A user without the admin role still sees only his own hub.
    const userMe = body<{ role: string; hubs: Array<{ hubId: string }> }>(
      await world.call('GET', '/v1/me'),
    );
    expect(userMe.role).toBe('user');
    expect(userMe.hubs.map((h) => h.hubId)).toEqual([HUB_ID]);
    const own = body<Array<{ camId: string }>>(await world.call('GET', '/v1/cameras'));
    expect(own.map((c) => c.camId)).toEqual([CAM_ID]);
    expect((await world.call('GET', '/v1/cameras/CAM_OTHER/media')).statusCode).toBe(404);
  });

  it('locks a user out until he gets a new token', async () => {
    const world = await galleryWorld();
    expect((await world.call('GET', '/v1/cameras')).statusCode).toBe(200);
    const revoked = await revokeAppUser(
      { admin: 'revokeAppUser', userId: world.user.userId },
      world.deps,
    );
    expect(revoked).toEqual({ ok: true, userId: world.user.userId, name: 'Opa' });
    expect((await world.call('GET', '/v1/cameras')).statusCode).toBe(401);
    expect((await revokeAppUser({ admin: 'revokeAppUser', userId: 1 }, world.deps)).ok).toBe(false);

    const renewed = await createAppUser(
      { admin: 'createAppUser', userId: world.user.userId },
      world.deps,
    );
    if (!renewed.ok) throw new Error(renewed.error);
    const res = await handleRequest(
      galleryRequest(renewed.token, 'GET', '/v1/cameras'),
      world.deps,
    );
    expect(body<Array<{ camId: string }>>(res).map((c) => c.camId)).toEqual([CAM_ID]);
  });
});

describe('admin view (#264)', () => {
  it('lists every hub with cameras and users, only for the admin', async () => {
    const world = await galleryWorld();
    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const asAdmin = (method: string, path: string, data?: unknown) =>
      handleRequest(galleryRequest(admin.token, method, path, data), world.deps);

    const res = await asAdmin('GET', '/v1/admin/hubs');
    expect(res.statusCode).toBe(200);
    const hubs =
      body<
        Array<{ hubId: string; cameras: Array<{ camId: string }>; users: Array<{ name?: string }> }>
      >(res);
    expect(hubs.map((h) => h.hubId)).toEqual([HUB_ID, 'other-hub']);
    expect(hubs[0]?.cameras.map((c) => c.camId)).toEqual([CAM_ID]);
    expect(hubs[0]?.users.map((u) => u.name)).toContain('Opa');
    expect(hubs[1]?.cameras).toEqual([]);
    // Faults (#265): the other hub never checked in.
    const faults = body<Array<{ faults: Array<{ kind: string }> }>>(res).map((h) =>
      h.faults.map((f) => f.kind),
    );
    expect(faults).toEqual([[], ['hubNeverSeen']]);

    expect((await world.call('GET', '/v1/admin/hubs')).statusCode).toBe(403);
  });

  it('takes a schedule by POST as well (CORS allows no PUT, #286)', async () => {
    const world = await galleryWorld();
    const res = await world.call('POST', `/v1/hubs/${HUB_ID}/schedule`, {
      cameraPlan: '*=120',
      checkinIntervalS: 3600,
    });
    expect(res.statusCode).toBe(200);
    expect(body(res)).toEqual({ cameraPlan: '*=120', checkinIntervalS: 3600 });
    expect((await world.call('POST', '/v1/hubs/other-hub/schedule', {})).statusCode).toBe(404);
  });
});

describe('deleting recordings from the gallery (#260)', () => {
  it('queues deletes, hands them to the hub and hides what the hub deleted', async () => {
    const { call, deps, hubKey } = await galleryWorld();
    const created = await call('POST', `/v1/cameras/${CAM_ID}/commands`, {
      type: 'deleteMedia',
      mediaIds: [2035, 2037],
    });
    expect(created.statusCode).toBe(201);
    let media = body<MediaItem[]>(await call('GET', `/v1/cameras/${CAM_ID}/media`));
    expect(media.map((m) => m.deleteState)).toEqual(['requested', 'requested']);

    const checkin = await handleRequest(
      hubRequest('/v1/device/checkin', hubKey, { cameras: [] }),
      deps,
    );
    const commands = body<CheckinResponse>(checkin).cameras[0]?.commands ?? [];
    expect(commands.map((c) => [c.type, c.mediaId])).toEqual([
      ['deleteMedia', 2035],
      ['deleteMedia', 2037],
    ]);

    await handleRequest(
      hubRequest('/v1/device/ack', hubKey, {
        cameras: [
          {
            camId: CAM_ID,
            commands: [
              { id: commands[0]!.id, state: 'done' },
              { id: commands[1]!.id, state: 'failed', error: 'unsupported command deleteMedia' },
            ],
          },
        ],
      }),
      deps,
    );
    media = body<MediaItem[]>(await call('GET', `/v1/cameras/${CAM_ID}/media`));
    expect(media).toHaveLength(1);
    expect(media[0]).toMatchObject({
      mediaId: 2037,
      deleteState: 'failed',
      deleteError: 'unsupported command deleteMedia',
    });
  });

  it('refuses unknown recordings and bad lists', async () => {
    const { call } = await galleryWorld();
    const path = `/v1/cameras/${CAM_ID}/commands`;
    expect((await call('POST', path, { type: 'deleteMedia', mediaIds: [9999] })).statusCode).toBe(
      404,
    );
    expect((await call('POST', path, { type: 'deleteMedia', mediaIds: [] })).statusCode).toBe(400);
    expect((await call('POST', path, { type: 'deleteMedia' })).statusCode).toBe(400);
  });

  it('withdraws a queued request, but not one the hub has received', async () => {
    const { call, deps, hubKey } = await galleryWorld();
    await call('POST', `/v1/cameras/${CAM_ID}/commands`, { type: 'requestVideo', mediaId: 2035 });
    const withdrawn = await call('POST', `/v1/cameras/${CAM_ID}/media/2035/withdraw`);
    expect(body(withdrawn)).toEqual({ withdrawn: 1 });
    let media = body<MediaItem[]>(await call('GET', `/v1/cameras/${CAM_ID}/media`));
    expect(media.find((m) => m.mediaId === 2035)?.videoState).toBe('none');

    await call('POST', `/v1/cameras/${CAM_ID}/commands`, { type: 'deleteMedia', mediaIds: [2037] });
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, { cameras: [] }), deps);
    expect((await call('POST', `/v1/cameras/${CAM_ID}/media/2037/withdraw`)).statusCode).toBe(409);
    media = body<MediaItem[]>(await call('GET', `/v1/cameras/${CAM_ID}/media`));
    expect(media.find((m) => m.mediaId === 2037)?.deleteState).toBe('delivered');
  });
});

describe('delete batches (#261)', () => {
  it('hands out at most 20 deletions per check-in, the rest later', async () => {
    const world = await setup();
    const media = Array.from({ length: 25 }, (_, i) => ({
      mediaId: 3000 + i,
      dir: 100,
      num: i + 1,
      fileType: 2,
      mediaTime: NOW - 1000 + i,
    }));
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, {
        cameras: [{ camId: CAM_ID, readAt: NOW - 60, media }],
      }),
      world.deps,
    );
    const user = await createAppUser(
      { admin: 'createAppUser', name: 'Opa', hubIds: [HUB_ID] },
      world.deps,
    );
    if (!user.ok) throw new Error(user.error);
    await handleRequest(
      galleryRequest(user.token, 'POST', `/v1/cameras/${CAM_ID}/commands`, {
        type: 'deleteMedia',
        mediaIds: media.map((m) => m.mediaId),
      }),
      world.deps,
    );

    const checkin = async () =>
      body<CheckinResponse>(
        await handleRequest(
          hubRequest('/v1/device/checkin', world.hubKey, { cameras: [] }),
          world.deps,
        ),
      ).cameras[0]?.commands ?? [];
    const first = await checkin();
    expect(first).toHaveLength(20);
    // Not answered yet: the same 20 come again, the other 5 still wait.
    expect((await checkin()).map((c) => c.id)).toEqual(first.map((c) => c.id));

    await handleRequest(
      hubRequest('/v1/device/ack', world.hubKey, {
        cameras: [{ camId: CAM_ID, commands: first.map((c) => ({ id: c.id, state: 'done' })) }],
      }),
      world.deps,
    );
    expect(await checkin()).toHaveLength(5);
  });
});

describe('battery history (#76)', () => {
  it('keeps one sample per hour for hub and camera and shows them to the admin', async () => {
    const world = await galleryWorld();
    let now = NOW;
    world.deps.now = () => now;
    const checkin = (readAt: number, batPercent: number, hubBatPercent: number) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, {
          hubBatPercent,
          cameras: [{ camId: CAM_ID, readAt, batPercent, sdFreeMB: 900, media: [] }],
        }),
        world.deps,
      );
    await checkin(now - 60, 80, 90);
    now += 600; // same hour, same read: nothing new
    await checkin(NOW - 60, 80, 89);
    now += 3600;
    await checkin(now - 30, 78, 85);

    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const res = await handleRequest(
      galleryRequest(admin.token, 'GET', `/v1/admin/hubs/${HUB_ID}/history`),
      world.deps,
    );
    expect(res.statusCode).toBe(200);
    const history = body<{
      hub: Array<{ batPercent?: number }>;
      cameras: Array<{ camId: string; samples: Array<{ batPercent?: number; sdFreeMB?: number }> }>;
    }>(res);
    expect(history.hub.map((s) => s.batPercent)).toEqual([90, 85]);
    expect(history.cameras[0]?.camId).toBe(CAM_ID);
    expect(history.cameras[0]?.samples.map((s) => s.batPercent)).toEqual([80, 78]);

    expect((await world.call('GET', `/v1/admin/hubs/${HUB_ID}/history`)).statusCode).toBe(403);
  });
});

describe('firmware changes (#149)', () => {
  it('records each new version and warns about a step back', async () => {
    const world = await galleryWorld();
    let now = NOW;
    world.deps.now = () => now;
    const checkin = (hubFirmware: string) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, { hubFirmware, cameras: [] }),
        world.deps,
      );
    await checkin('v0.1.12');
    now += 60;
    await checkin('v0.1.12');
    now += 60;
    await checkin('v0.1.13');
    now += 60;
    await checkin('v0.1.12');

    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const hubs = body<
      Array<{
        hubId: string;
        firmwareChanges: Array<{ from: string; to: string }>;
        faults: Array<{ kind: string }>;
      }>
    >(await handleRequest(galleryRequest(admin.token, 'GET', '/v1/admin/hubs'), world.deps));
    const hub = hubs.find((h) => h.hubId === HUB_ID);
    expect(hub?.firmwareChanges.map((c) => `${c.from}>${c.to}`)).toEqual([
      'v0.1.13>v0.1.12',
      'v0.1.12>v0.1.13',
    ]);
    expect(hub?.faults.map((f) => f.kind)).toContain('firmwareDowngrade');
  });
});

describe('transport and modem signal (#149)', () => {
  it('stores them and shows the signal only over the modem', async () => {
    const world = await galleryWorld();
    const checkin = (body: Record<string, unknown>) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], ...body }),
        world.deps,
      );
    await checkin({ hubUplink: 'modem', modemSignalDbm: -87 });
    let hub = await world.deps.store.getHub(HUB_ID);
    expect(hub).toMatchObject({ uplink: 'modem', modemSignalDbm: -87 });
    await checkin({ hubUplink: 'satellite', modemSignalDbm: 5 });
    hub = await world.deps.store.getHub(HUB_ID);
    expect(hub).toMatchObject({ uplink: 'modem', modemSignalDbm: -87 });
    await checkin({ hubUplink: 'wifi' });
    hub = await world.deps.store.getHub(HUB_ID);
    expect(hub?.uplink).toBe('wifi');
  });
});

describe("the hub's own update record (#149)", () => {
  it('stores each report once and warns about a rollback', async () => {
    const world = await galleryWorld();
    let now = NOW;
    world.deps.now = () => now;
    const checkin = (hubFirmware: string, firmwareUpdate?: unknown) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, {
          hubFirmware,
          firmwareUpdate,
          cameras: [],
        }),
        world.deps,
      );
    await checkin('v0.1.12');
    now += 60;
    // Installed and reported by the hub: one entry, not two.
    await checkin('v0.1.13', { from: 'v0.1.12', to: 'v0.1.13', result: 'installed', at: now - 5 });
    now += 60;
    // A rollback without a clock: the check-in's time.
    await checkin('v0.1.13', {
      from: 'v0.1.13',
      to: 'v0.1.14',
      result: 'rolledBack',
      reason: 'no check-in',
    });
    now += 60;
    // Malformed reports are ignored.
    await checkin('v0.1.13', { from: 'v0.1.13', result: 'installed' });
    await checkin('v0.1.13', { from: 'a', to: 'b', result: 'exploded' });

    const changes = await world.deps.store.listFirmwareChanges(HUB_ID, 10);
    expect(changes).toEqual([
      {
        at: NOW + 120,
        from: 'v0.1.13',
        to: 'v0.1.14',
        result: 'rolledBack',
        reason: 'no check-in',
      },
      { at: NOW + 55, from: 'v0.1.12', to: 'v0.1.13', result: 'installed' },
    ]);
    const hub = await world.deps.store.getHub(HUB_ID);
    if (!hub) throw new Error('no hub');
    const faults = hubFaults(hub, [], now, changes);
    expect(faults.find((f) => f.kind === 'firmwareUpdateFailed')).toMatchObject({
      value: 1,
      detail: 'v0.1.13 → v0.1.14: no check-in',
    });
  });
});

describe('video requests on a hub without PSRAM (#244)', () => {
  it('hands out as many as thumbnail uploads the hub asks for', async () => {
    const world = await galleryWorld();
    for (const mediaId of [2035, 2037]) {
      const res = await world.call('POST', `/v1/cameras/${CAM_ID}/commands`, {
        type: 'requestVideo',
        mediaId,
      });
      expect(res.statusCode).toBe(201);
    }
    const videos = async (extra: Record<string, unknown>) =>
      (
        body<CheckinResponse>(
          await handleRequest(
            hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], ...extra }),
            world.deps,
          ),
        ).cameras[0]?.commands ?? []
      ).filter((c) => c.type === 'requestVideo');

    const small = await videos({ maxThumbUploads: 1 });
    expect(small).toHaveLength(1);
    // The delivered one comes again first; at least one even with 0 uploads.
    expect((await videos({ maxThumbUploads: 0 })).map((c) => c.id)).toEqual(small.map((c) => c.id));
    expect(await videos({})).toHaveLength(2);
  });
});

describe('hiding a camera (#273)', () => {
  it('lets a user hide a camera and show it again', async () => {
    const { call } = await galleryWorld();
    const hide = await call('POST', `/v1/cameras/${CAM_ID}/hidden`, { hidden: true });
    expect(hide.statusCode).toBe(200);
    expect(body(hide)).toEqual({ camId: CAM_ID, hidden: true });
    const listed = body<Array<{ camId: string; hidden?: boolean }>>(
      await call('GET', '/v1/cameras'),
    );
    expect(listed).toEqual([expect.objectContaining({ camId: CAM_ID, hidden: true })]);
    // The recordings stay.
    expect((await call('GET', `/v1/cameras/${CAM_ID}/media`)).statusCode).toBe(200);

    await call('POST', `/v1/cameras/${CAM_ID}/hidden`, { hidden: false });
    const shown = body<Array<{ hidden?: boolean }>>(await call('GET', '/v1/cameras'));
    expect(shown[0]?.hidden).toBeUndefined();

    expect((await call('POST', `/v1/cameras/${CAM_ID}/hidden`, { hidden: 'yes' })).statusCode).toBe(
      400,
    );
    expect((await call('POST', '/v1/cameras/nope/hidden', { hidden: true })).statusCode).toBe(404);
  });
});

describe('found cameras (#296, #297)', () => {
  const A = 'AA:BB:CC:00:00:01';
  const B = 'F4:C1:56:06:ED:3B';

  it('notes found cameras, approves read ones and answers with the approved list', async () => {
    const world = await galleryWorld();
    const checkin = async (data: Record<string, unknown>) =>
      body<{ approvedCameras?: string[] }>(
        await handleRequest(hubRequest('/v1/device/checkin', world.hubKey, data), world.deps),
      );

    // A hub without BLE addresses gets no list: it reads every camera.
    expect((await checkin({ cameras: [] })).approvedCameras).toBeUndefined();

    const first = await checkin({
      discovered: [
        { bleAddress: 'aa-bb-cc-00-00-01', name: 'TC100_0001', rssi: -70, seenAt: NOW - 30 },
        { bleAddress: B, name: 'TC100_ED3B', rssi: -55 },
      ],
      cameras: [{ camId: CAM_ID, readAt: NOW - 20, bleAddress: B, media: [] }],
    });
    expect(first.approvedCameras).toEqual([B]);

    const listed = body<Array<{ bleAddress: string; state: string; camId?: string; name: string }>>(
      await world.call('GET', `/v1/hubs/${HUB_ID}/found-cameras`),
    );
    expect(listed.map((c) => [c.bleAddress, c.state, c.camId, c.name])).toEqual([
      [A, 'new', undefined, 'TC100_0001'],
      [B, 'approved', CAM_ID, CAM_ID],
    ]);

    // The user takes the new one in with a name; it becomes the camera's name at its first read.
    const approve = await world.call('POST', `/v1/hubs/${HUB_ID}/found-cameras/${A}`, {
      state: 'approved',
      name: 'Futterplatz',
    });
    expect(approve.statusCode).toBe(200);
    expect((await checkin({ discovered: [] })).approvedCameras).toEqual([A, B]);
    await checkin({ cameras: [{ camId: 'CAM_A', readAt: NOW - 10, bleAddress: A, media: [] }] });
    const cameras = body<Array<{ camId: string; name: string }>>(
      await world.call('GET', '/v1/cameras'),
    );
    expect(cameras.find((c) => c.camId === 'CAM_A')?.name).toBe('Futterplatz');

    // Stop reading one: it stays excluded even when an old hub reads it again.
    await world.call('POST', `/v1/hubs/${HUB_ID}/found-cameras/${B.replace(/:/g, '')}`, {
      state: 'excluded',
    });
    const after = await checkin({
      cameras: [{ camId: CAM_ID, readAt: NOW - 5, bleAddress: B, media: [] }],
    });
    expect(after.approvedCameras).toEqual([A]);
    // ... and can be taken in again later.
    await world.call('POST', `/v1/hubs/${HUB_ID}/found-cameras/${B}`, { state: 'approved' });
    expect((await checkin({ discovered: [] })).approvedCameras).toEqual([A, B]);
  });

  it('checks the input and who may decide', async () => {
    const world = await galleryWorld();
    const bad = await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { discovered: [{ bleAddress: 'nope' }] }),
      world.deps,
    );
    expect(bad.statusCode).toBe(400);
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { discovered: [{ bleAddress: A }] }),
      world.deps,
    );
    const path = `/v1/hubs/${HUB_ID}/found-cameras/${A}`;
    expect((await world.call('POST', path, { state: 'maybe' })).statusCode).toBe(400);
    expect((await world.call('POST', path, { state: 'approved', name: '' })).statusCode).toBe(400);
    expect(
      (await world.call('POST', `/v1/hubs/${HUB_ID}/found-cameras/${B}`, { state: 'approved' }))
        .statusCode,
    ).toBe(404);
    expect((await world.call('GET', '/v1/hubs/other-hub/found-cameras')).statusCode).toBe(404);
  });
});

describe('asking the hub to look for cameras (#308)', () => {
  it('asks for a read of every camera that is not excluded', async () => {
    const world = await galleryWorld();
    const res = await world.call('POST', `/v1/hubs/${HUB_ID}/scan`);
    expect(res.statusCode).toBe(200);
    expect(body<{ requested: number }>(res).requested).toBe(1);
    const cameras = body<Array<{ readRequested: boolean }>>(await world.call('GET', '/v1/cameras'));
    expect(cameras[0]?.readRequested).toBe(true);
    // Asking again does not queue a second read.
    expect(
      body<{ requested: number }>(await world.call('POST', `/v1/hubs/${HUB_ID}/scan`)).requested,
    ).toBe(1);

    // An excluded camera is not read.
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, {
        cameras: [{ camId: CAM_ID, readAt: NOW - 5, bleAddress: 'F4:C1:56:06:ED:3B', media: [] }],
      }),
      world.deps,
    );
    await world.call('POST', `/v1/hubs/${HUB_ID}/found-cameras/F4C15606ED3B`, {
      state: 'excluded',
    });
    expect(
      body<{ requested: number }>(await world.call('POST', `/v1/hubs/${HUB_ID}/scan`)).requested,
    ).toBe(0);
    expect((await world.call('POST', '/v1/hubs/other-hub/scan')).statusCode).toBe(404);
  });
});

describe('camera names (#273)', () => {
  it('shows the name the admin gave the camera', async () => {
    const { call, deps } = await galleryWorld();
    const { renameCamera } = await import('./admin');
    expect(
      await renameCamera({ admin: 'renameCamera', camId: CAM_ID, name: ' Garten ' }, deps),
    ).toEqual({
      ok: true,
      camId: CAM_ID,
      name: 'Garten',
    });
    const cameras = body<Array<{ camId: string; name: string }>>(await call('GET', '/v1/cameras'));
    expect(cameras[0]).toMatchObject({ camId: CAM_ID, name: 'Garten' });

    expect((await renameCamera({ admin: 'renameCamera', camId: CAM_ID, name: '' }, deps)).ok).toBe(
      false,
    );
    expect((await renameCamera({ admin: 'renameCamera', camId: 'nope', name: 'X' }, deps)).ok).toBe(
      false,
    );
  });
  it('lets a user of the hub rename the camera in the gallery', async () => {
    const { call, deps } = await galleryWorld();
    const renamed = await call('POST', `/v1/cameras/${CAM_ID}/name`, { name: '  Futterplatz ' });
    expect(renamed.statusCode).toBe(200);
    expect(body(renamed)).toEqual({ camId: CAM_ID, name: 'Futterplatz' });
    const cameras = body<Array<{ camId: string; name: string }>>(await call('GET', '/v1/cameras'));
    expect(cameras[0]).toMatchObject({ camId: CAM_ID, name: 'Futterplatz' });

    for (const name of ['', '   ', 'x'.repeat(41), 'a\nb', 42]) {
      const bad = await call('POST', `/v1/cameras/${CAM_ID}/name`, { name });
      expect(bad.statusCode).toBe(400);
    }
    expect((await call('POST', `/v1/cameras/${CAM_ID}/name`, null)).statusCode).toBe(400);
    expect((await call('POST', '/v1/cameras/nope/name', { name: 'X' })).statusCode).toBe(404);

    // A user without a link to the camera's hub gets the same 404.
    const stranger = await createAppUser({ admin: 'createAppUser', name: 'Fremd' }, deps);
    if (!stranger.ok) throw new Error(stranger.error);
    const res = await handleRequest(
      galleryRequest(stranger.token, 'POST', `/v1/cameras/${CAM_ID}/name`, { name: 'Meins' }),
      deps,
    );
    expect(res.statusCode).toBe(404);
    expect(body<Array<{ name: string }>>(await call('GET', '/v1/cameras'))[0]?.name).toBe(
      'Futterplatz',
    );
  });
});
