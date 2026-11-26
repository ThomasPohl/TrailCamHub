import { describe, expect, it } from 'vitest';
import { hubFaults } from './faults';
import { createAppUser, revokeAppUser } from './admin';
import { createAdminSession } from './auth/admin-session';
import { chargingSample, handleRequest, lowestRecently } from './api';
import type { ApiRequest, ApiResponse } from './http';
import type { Camera, CheckinRequest, CheckinResponse, MediaItem } from './model';
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

function adminRequest(session: string, method: string, path: string, data?: unknown): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `Admin ${session}` },
    body: data === undefined ? undefined : JSON.stringify(data),
  };
}

/** An admin session (#259) for a user created with `createAppUser`. */
async function adminSession(world: Awaited<ReturnType<typeof setup>>, userId: number) {
  const record = await world.store.getAppUser(userId);
  if (!record) throw new Error('unknown user');
  return createAdminSession(record, world.deps.now());
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
    const session = await adminSession(world, admin.userId);
    const asAdmin = (method: string, path: string, data?: unknown) =>
      handleRequest(adminRequest(session, method, path, data), world.deps);

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
    const session = await adminSession(world, admin.userId);
    const asAdmin = (method: string, path: string, data?: unknown) =>
      handleRequest(adminRequest(session, method, path, data), world.deps);

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

/** The gallery world with a hub whose firmware knows delete targets (#461). */
async function targetsWorld() {
  const world = await galleryWorld();
  await handleRequest(
    hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], features: ['deleteTargets'] }),
    world.deps,
  );
  const path = `/v1/cameras/${CAM_ID}/commands`;
  const del = (mediaIds: number[], targets?: string[]) =>
    world.call('POST', path, { type: 'deleteMedia', mediaIds, ...(targets ? { targets } : {}) });
  const media = async () =>
    body<MediaItem[]>(await world.call('GET', `/v1/cameras/${CAM_ID}/media`));
  const checkin = async (extra: object = { cameras: [] }) =>
    body<CheckinResponse>(
      await handleRequest(hubRequest('/v1/device/checkin', world.hubKey, extra), world.deps),
    );
  const ack = async (id: string, state: 'done' | 'failed') =>
    handleRequest(
      hubRequest('/v1/device/ack', world.hubKey, {
        cameras: [{ camId: CAM_ID, commands: [{ id, state }] }],
      }),
      world.deps,
    );
  return { ...world, del, media, checkin, ack };
}

describe('delete targets (#461)', () => {
  it('shows the targets of the hub: the camera for an old firmware, hub and gallery for a new one', async () => {
    const world = await galleryWorld();
    const camera = async () =>
      body<Camera[]>(await world.call('GET', '/v1/cameras')).find((c) => c.camId === CAM_ID);
    // The firmware has not reported the feature: it deletes on the camera, as before.
    expect(await camera()).toMatchObject({
      hubDeleteTargets: ['camera', 'backend'],
      hubDeletesByTarget: false,
    });
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], features: ['deleteTargets'] }),
      world.deps,
    );
    expect(await camera()).toMatchObject({
      hubDeleteTargets: ['hub', 'backend'],
      hubDeletesByTarget: true,
    });
    // A later firmware without the feature (a rollback) does not keep it.
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { cameras: [] }),
      world.deps,
    );
    expect(await camera()).toMatchObject({ hubDeletesByTarget: false });
  });

  it('lets any user of the hub set the targets, the same for everybody', async () => {
    const world = await targetsWorld();
    const set = (targets: unknown, hub = HUB_ID) =>
      world.call('POST', `/v1/hubs/${hub}/delete-targets`, { targets });
    expect((await set(['hub', 'camera', 'backend'])).statusCode).toBe(200);
    const cameras = body<Camera[]>(await world.call('GET', '/v1/cameras'));
    // In the order of the list of targets, whatever the order sent.
    expect(cameras[0]?.hubDeleteTargets).toEqual(['camera', 'hub', 'backend']);
    expect((await world.store.getHub(HUB_ID))?.deleteTargets).toEqual(['camera', 'hub', 'backend']);
    for (const bad of [[], ['floppy'], ['hub', 'hub'], 'hub', undefined]) {
      expect((await set(bad)).statusCode).toBe(400);
    }
    expect((await set(['hub'], 'other-hub')).statusCode).toBe(404);
  });

  it('uses the hub setting when the request has no targets', async () => {
    const world = await targetsWorld();
    await world.call('POST', `/v1/hubs/${HUB_ID}/delete-targets`, { targets: ['camera'] });
    await world.del([2035]);
    const commands = (await world.checkin()).cameras[0]?.commands ?? [];
    expect(commands).toMatchObject([{ type: 'deleteMedia', mediaId: 2035, targets: ['camera'] }]);
  });

  it('refuses bad targets in a request', async () => {
    const world = await targetsWorld();
    for (const bad of [[], ['floppy'], ['hub', 'hub'], 'hub']) {
      expect((await world.del([2035], bad as string[])).statusCode).toBe(400);
    }
  });

  it('deletes in the gallery alone at once, without a command for the hub', async () => {
    const world = await targetsWorld();
    world.storage.uploaded.add(`${CAM_ID}/2035`);
    const res = await world.del([2035], ['backend']);
    expect(res.statusCode).toBe(200);
    expect(body(res)).toEqual([]);
    expect((await world.media()).map((m) => m.mediaId)).toEqual([2037]);
    expect(world.storage.deleted).toEqual([`${CAM_ID}/2035`]);
    expect((await world.checkin()).cameras).toEqual([]);

    // The hub still has it and reports it again: it does not come back, and
    // no thumbnail is asked for.
    const again = await world.checkin(checkinBody);
    expect((await world.media()).map((m) => m.mediaId)).toEqual([2037]);
    expect(again.cameras.flatMap((c) => c.thumbUploads.map((t) => t.mediaId))).not.toContain(2035);
    // It cannot be asked for again.
    expect((await world.del([2035], ['backend'])).statusCode).toBe(404);
  });

  it('hands camera and hub to the hub and deletes in the gallery when it is done', async () => {
    const world = await targetsWorld();
    await world.del([2035], ['camera', 'hub', 'backend']);
    const commands = (await world.checkin()).cameras[0]?.commands ?? [];
    // The gallery's part is not the hub's business.
    expect(commands).toMatchObject([{ mediaId: 2035, targets: ['camera', 'hub'] }]);
    expect((await world.media()).map((m) => [m.mediaId, m.deleteState])).toEqual([
      [2037, undefined],
      [2035, 'delivered'],
    ]);
    expect(world.storage.deleted).toEqual([]);

    await world.ack(commands[0]!.id, 'done');
    expect((await world.media()).map((m) => m.mediaId)).toEqual([2037]);
    expect(world.storage.deleted).toEqual([`${CAM_ID}/2035`]);
  });

  it('keeps everything where it is when the hub fails', async () => {
    const world = await targetsWorld();
    await world.del([2035], ['camera', 'hub', 'backend']);
    const commands = (await world.checkin()).cameras[0]?.commands ?? [];
    await world.ack(commands[0]!.id, 'failed');
    const item = (await world.media()).find((m) => m.mediaId === 2035);
    expect(item).toMatchObject({ deleteState: 'failed' });
    expect(world.storage.deleted).toEqual([]);
  });

  it('keeps the recording in the gallery after a delete on the camera alone', async () => {
    const world = await targetsWorld();
    await world.del([2035], ['camera']);
    const commands = (await world.checkin()).cameras[0]?.commands ?? [];
    expect(commands).toMatchObject([{ mediaId: 2035, targets: ['camera'] }]);
    // The hub reports the file gone from the card, in the same check-in as the
    // answer or before it.
    await world.checkin({
      cameras: [{ camId: CAM_ID, media: [], removedMedia: [2035] }],
    });
    await world.ack(commands[0]!.id, 'done');
    await world.checkin({
      cameras: [{ camId: CAM_ID, media: [], removedMedia: [2035] }],
    });
    const item = (await world.media()).find((m) => m.mediaId === 2035);
    expect(item).toMatchObject({ mediaId: 2035, onCamera: false });
    expect(item?.deleteState).toBeUndefined();
    expect(world.storage.deleted).toEqual([]);
    // A video cannot be fetched from a camera that no longer has it.
    expect(
      (
        await world.call('POST', `/v1/cameras/${CAM_ID}/commands`, {
          type: 'requestVideo',
          mediaId: 2035,
        })
      ).statusCode,
    ).toBe(409);
  });

  it('leaves the gallery alone when only the hub deletes its files', async () => {
    const world = await targetsWorld();
    await world.del([2035], ['hub']);
    const commands = (await world.checkin()).cameras[0]?.commands ?? [];
    expect(commands).toMatchObject([{ mediaId: 2035, targets: ['hub'] }]);
    await world.ack(commands[0]!.id, 'done');
    const item = (await world.media()).find((m) => m.mediaId === 2035);
    expect(item).toBeDefined();
    expect(item?.deleteState).toBeUndefined();
    expect(item?.onCamera).toBeUndefined();
    expect(world.storage.deleted).toEqual([]);
  });

  it('deletes what the camera lost by itself everywhere, whatever the setting', async () => {
    const world = await targetsWorld();
    await world.call('POST', `/v1/hubs/${HUB_ID}/delete-targets`, { targets: ['camera'] });
    await world.checkin({ cameras: [{ camId: CAM_ID, media: [], removedMedia: [2035] }] });
    expect((await world.media()).map((m) => m.mediaId)).toEqual([2037]);
    expect(world.storage.deleted).toEqual([`${CAM_ID}/2035`]);
    // Reported twice, deleted once.
    await world.checkin({ cameras: [{ camId: CAM_ID, media: [], removedMedia: [2035] }] });
    expect(world.storage.deleted).toEqual([`${CAM_ID}/2035`]);
  });

  it('takes a failed delete in the storage as done: the recording is gone for the user', async () => {
    const world = await targetsWorld();
    world.storage.deleteMedia = async () => {
      throw new Error('S3 is down');
    };
    const logged = console.error;
    console.error = () => {};
    try {
      expect((await world.del([2035], ['backend'])).statusCode).toBe(200);
    } finally {
      console.error = logged;
    }
    expect((await world.media()).map((m) => m.mediaId)).toEqual([2037]);
  });

  it('gives a firmware without targets only what it can do: delete on the camera', async () => {
    const world = await galleryWorld();
    const path = `/v1/cameras/${CAM_ID}/commands`;
    const del = (targets: string[]) =>
      world.call('POST', path, { type: 'deleteMedia', mediaIds: [2035], targets });
    for (const targets of [['hub'], ['hub', 'backend'], ['camera', 'hub']]) {
      expect((await del(targets)).statusCode).toBe(409);
    }
    expect(
      (await world.store.listCommands(CAM_ID)).filter((c) => c.type === 'deleteMedia'),
    ).toEqual([]);
    expect((await del(['camera', 'backend'])).statusCode).toBe(201);
    // The same without targets: the setting of such a hub is the camera and the gallery.
    expect(
      (await world.call('POST', path, { type: 'deleteMedia', mediaIds: [2037] })).statusCode,
    ).toBe(201);
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
  it('forgets the hub battery when a check-in no longer reports it', async () => {
    const world = await galleryWorld();
    const checkin = (body: object) =>
      handleRequest(hubRequest('/v1/device/checkin', world.hubKey, body), world.deps);
    await checkin({ hubBatPercent: 65 });
    expect((await world.deps.store.getHub(HUB_ID))?.batPercent).toBe(65);
    // The gauge does not answer: no value, and no stale 65 % in the admin view.
    await checkin({});
    expect((await world.deps.store.getHub(HUB_ID))?.batPercent).toBeUndefined();
    await checkin({ hubBatPercent: 60 });
    expect((await world.deps.store.getHub(HUB_ID))?.batPercent).toBe(60);
  });

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
      adminRequest(
        await adminSession(world, admin.userId),
        'GET',
        `/v1/admin/hubs/${HUB_ID}/history`,
      ),
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
    >(
      await handleRequest(
        adminRequest(await adminSession(world, admin.userId), 'GET', '/v1/admin/hubs'),
        world.deps,
      ),
    );
    const hub = hubs.find((h) => h.hubId === HUB_ID);
    expect(hub?.firmwareChanges.map((c) => `${c.from}>${c.to}`)).toEqual([
      'v0.1.13>v0.1.12',
      'v0.1.12>v0.1.13',
    ]);
    expect(hub?.faults.map((f) => f.kind)).toContain('firmwareDowngrade');
  });
});

describe('memory report (#149, #244)', () => {
  it('stores a plausible report and warns about little heap', async () => {
    const world = await galleryWorld();
    const checkin = (memory: unknown) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], memory }),
        world.deps,
      );
    await checkin({ cycle: 12, heapMin: 6800, stackSession: 900, psramMin: -1, extra: 1 });
    const hub = await world.deps.store.getHub(HUB_ID);
    expect(hub?.memory).toEqual({ cycle: 12, heapMin: 6800, stackSession: 900 });
    if (!hub) throw new Error('no hub');
    expect(hubFaults(hub, [], NOW, []).find((f) => f.kind === 'hubMemoryLow')).toMatchObject({
      value: 6800,
    });
    await checkin({ heapMin: 'lots' });
    expect((await world.deps.store.getHub(HUB_ID))?.memory?.cycle).toBe(12);
  });
});

describe('charging state of the hub (#421)', () => {
  it('stores the flag and the voltage with the battery sample, ignoring odd values', () => {
    expect(chargingSample({ hubCharging: true, hubBatMillivolts: 4187 })).toEqual({
      charging: true,
      millivolts: 4187,
    });
    expect(chargingSample({ hubCharging: false })).toEqual({ charging: false });
    expect(chargingSample({ hubCharging: 'yes', hubBatMillivolts: 12 } as never)).toEqual({});
    expect(chargingSample({ hubBatMillivolts: 4187.5 })).toEqual({});
  });
});

describe('charging state of a camera (#473)', () => {
  it('stores the flag with the camera sample and leaves it out when the hub sends none', async () => {
    const world = await galleryWorld();
    const read = (readAt: number, extra: Record<string, unknown>) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, {
          cameras: [{ camId: CAM_ID, readAt, batPercent: 100, media: [], ...extra }],
        }),
        world.deps,
      );
    // One sample per hour at most, so the reads are an hour apart, oldest first.
    await read(NOW - 3 * 3700, { isCharging: true });
    await read(NOW - 2 * 3700, { isCharging: 'yes' });
    await read(NOW - 3700, {});
    const samples = await world.deps.store.listSamples({ camId: CAM_ID }, 0);
    expect(samples.map((x) => x.charging)).toEqual([true, undefined, undefined]);
  });
});

describe('lowest camera battery value of 12 h (#472)', () => {
  it('keeps a camera that toggles at a step low, and lets a charge clear it', async () => {
    const world = await galleryWorld();
    const sample = (at: number, batPercent: number, charging?: boolean) =>
      world.deps.store.addSample({ camId: CAM_ID }, { at, batPercent, charging });
    await sample(NOW - 5 * 3600, 20);
    await sample(NOW - 2 * 3600, 80);
    const low = (percent: number | undefined) => lowestRecently(world.deps, CAM_ID, NOW, percent);
    expect(await low(80)).toBe(20);
    expect(await low(20)).toBe(20);
    // Older than 12 h does not count.
    await world.deps.store.addSample({ camId: CAM_ID }, { at: NOW - 13 * 3600, batPercent: 0 });
    expect(await low(80)).toBe(20);
    // A full value stands alone, and so does what comes after a flagged charge.
    expect(await low(100)).toBe(100);
    await sample(NOW - 1 * 3600, 90, true);
    expect(await low(80)).toBe(80);
    expect(await low(undefined)).toBeUndefined();
  });
});

describe('power mode (#416)', () => {
  it('stores the mode, ignores other values and drops it when it is no longer sent', async () => {
    const world = await galleryWorld();
    const checkin = (body: Record<string, unknown>) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], ...body }),
        world.deps,
      );
    const mode = async () => (await world.deps.store.getHub(HUB_ID))?.powerMode;
    await checkin({ hubPowerMode: 'mains' });
    expect(await mode()).toBe('mains');
    await checkin({ hubPowerMode: 'solar' });
    expect(await mode()).toBeUndefined();
    await checkin({ hubPowerMode: 'battery' });
    expect(await mode()).toBe('battery');
    await checkin({});
    expect(await mode()).toBeUndefined();
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

  it('stores the mobile network, which older firmware does not send', async () => {
    const world = await galleryWorld();
    const checkin = (body: Record<string, unknown>) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, { cameras: [], ...body }),
        world.deps,
      );
    const operator = async () => (await world.deps.store.getHub(HUB_ID))?.modemOperator;
    // No field: nothing stored, the check-in is accepted.
    expect((await checkin({ hubUplink: 'modem', modemSignalDbm: -87 })).statusCode).toBe(200);
    expect(await operator()).toBeUndefined();
    await checkin({ hubUplink: 'modem', modemOperator: '  Telekom.de\n' });
    expect(await operator()).toBe('Telekom.de');
    // Only over the modem, and only text.
    await checkin({ hubUplink: 'wifi', modemOperator: 'Vodafone.de' });
    expect(await operator()).toBe('Telekom.de');
    await checkin({ hubUplink: 'modem', modemOperator: 42 });
    expect(await operator()).toBe('Telekom.de');
    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const hubs = body<Array<{ hubId: string; uplink?: string; modemOperator?: string }>>(
      await handleRequest(
        adminRequest(await adminSession(world, admin.userId), 'GET', '/v1/admin/hubs'),
        world.deps,
      ),
    );
    expect(hubs.find((h) => h.hubId === HUB_ID)).toMatchObject({
      uplink: 'modem',
      modemOperator: 'Telekom.de',
    });
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
    // A held-back release: an entry with the reason, and no fault of its own.
    await checkin('v0.1.13', {
      from: 'v0.1.13',
      to: 'v0.1.15',
      result: 'skipped',
      reason: 'the hub battery is too low',
      at: now - 2,
    });
    now += 60;
    // Malformed reports are ignored.
    await checkin('v0.1.13', { from: 'v0.1.13', result: 'installed' });
    await checkin('v0.1.13', { from: 'a', to: 'b', result: 'exploded' });

    const changes = await world.deps.store.listFirmwareChanges(HUB_ID, 10);
    expect(changes).toEqual([
      {
        at: NOW + 178,
        from: 'v0.1.13',
        to: 'v0.1.15',
        result: 'skipped',
        reason: 'the hub battery is too low',
      },
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

  it('leaves a hidden camera out of the approved list (#273)', async () => {
    const world = await galleryWorld();
    const checkin = async () =>
      body<{ approvedCameras?: string[] }>(
        await handleRequest(
          hubRequest('/v1/device/checkin', world.hubKey, {
            cameras: [{ camId: CAM_ID, readAt: NOW - 20, bleAddress: B, media: [] }],
          }),
          world.deps,
        ),
      );
    expect((await checkin()).approvedCameras).toEqual([B]);

    await world.call('POST', `/v1/cameras/${CAM_ID}/hidden`, { hidden: true });
    expect((await checkin()).approvedCameras).toEqual([]);

    await world.call('POST', `/v1/cameras/${CAM_ID}/hidden`, { hidden: false });
    expect((await checkin()).approvedCameras).toEqual([B]);
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

  it('stores a skipped record without a release (#363)', async () => {
    const world = await galleryWorld();
    const send = (firmwareUpdate: unknown) =>
      handleRequest(
        hubRequest('/v1/device/checkin', world.hubKey, {
          hubFirmware: 'v0.1.13',
          firmwareUpdate,
          cameras: [],
        }),
        world.deps,
      );
    await send({ from: 'v0.1.13', result: 'skipped', reason: 'the hub battery is too low' });
    // Only a skipped record may come without `to`.
    await send({ from: 'v0.1.13', result: 'failed', reason: 'x' });
    const changes = await world.deps.store.listFirmwareChanges(HUB_ID, 10);
    expect(changes).toEqual([
      {
        at: NOW,
        from: 'v0.1.13',
        to: '',
        result: 'skipped',
        reason: 'the hub battery is too low',
      },
    ]);
  });
});

describe('what a user may do and see (#431)', () => {
  const schedule = `/v1/hubs/${HUB_ID}/schedule`;

  async function adminOf(world: Awaited<ReturnType<typeof galleryWorld>>) {
    const admin = await createAppUser(
      { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
      world.deps,
    );
    if (!admin.ok) throw new Error(admin.error);
    const session = await adminSession(world, admin.userId);
    return (method: string, path: string, data?: unknown) =>
      handleRequest(adminRequest(session, method, path, data), world.deps);
  }

  it('lets a user set wake times and the contact interval, but not the power supply', async () => {
    const world = await galleryWorld();
    const ok = await world.call('POST', schedule, { cameraPlan: '*=120', checkinIntervalS: 900 });
    expect(ok.statusCode).toBe(200);
    const denied = await world.call('POST', schedule, { powerSetting: 'battery' });
    expect(denied.statusCode).toBe(403);
    expect((await world.store.getHub(HUB_ID))?.scheduleConfig).toEqual({
      cameraPlan: '*=120',
      checkinIntervalS: 900,
    });
  });

  it('lets the admin set the power supply, which a later user change keeps', async () => {
    const world = await galleryWorld();
    const asAdmin = await adminOf(world);
    expect((await asAdmin('POST', schedule, { powerSetting: 'battery' })).statusCode).toBe(200);
    const res = await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, checkinBody),
      world.deps,
    );
    expect(body<CheckinResponse>(res).config).toEqual({ powerSetting: 'battery' });
    // Null hands it back: the hub keeps what it has.
    await asAdmin('POST', schedule, { powerSetting: null, checkinIntervalS: 900 });
    expect((await world.store.getHub(HUB_ID))?.scheduleConfig).toEqual({ checkinIntervalS: 900 });
    for (const bad of ['solar', 'MAINS', 1, true]) {
      expect((await asAdmin('POST', schedule, { powerSetting: bad })).statusCode).toBe(400);
    }

    await asAdmin('POST', schedule, { powerSetting: 'mains' });
    const user = await world.call('POST', schedule, { cameraPlan: '*=60' });
    expect(user.statusCode).toBe(200);
    expect(body(user)).toEqual({ cameraPlan: '*=60', powerSetting: 'mains' });
  });

  it('shows a user the history and the data volume of his own hub only', async () => {
    const world = await galleryWorld();
    const history = await world.call('GET', `/v1/hubs/${HUB_ID}/history`);
    expect(history.statusCode).toBe(200);
    expect(
      body<{ cameras: Array<{ camId: string }> }>(history).cameras.map((c) => c.camId),
    ).toEqual([CAM_ID]);
    const usage = await world.call('GET', `/v1/hubs/${HUB_ID}/usage`);
    expect(usage.statusCode).toBe(200);
    expect(body<{ days: unknown[] }>(usage).days).toEqual(expect.any(Array));
    for (const what of ['history', 'usage', 'hints']) {
      expect((await world.call('GET', `/v1/hubs/other-hub/${what}`)).statusCode).toBe(404);
    }
  });

  it('gives the user the hints he can act on, not the technical faults', async () => {
    const world = await galleryWorld();
    // A full SD card is a hint; a failed request is the admin's business.
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, {
        cameras: [
          {
            camId: CAM_ID,
            readAt: NOW - 60,
            sdCard: 'ok',
            sdFreeMB: 100,
            sdTotalMB: 30_000,
            media: [],
          },
        ],
      }),
      world.deps,
    );
    const res = await world.call('GET', `/v1/hubs/${HUB_ID}/hints`);
    expect(res.statusCode).toBe(200);
    const kinds = body<Array<{ kind: string }>>(res).map((f) => f.kind);
    expect(kinds).toContain('sdFull');
    for (const technical of ['requestsFailed', 'firmwareDowngrade', 'hubMemoryLow']) {
      expect(kinds).not.toContain(technical);
    }
  });
});
