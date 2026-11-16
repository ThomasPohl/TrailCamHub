import { describe, expect, it } from 'vitest';
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

    const me = body<{ role: string; name: string }>(await asAdmin('GET', '/v1/me'));
    expect(me).toMatchObject({ role: 'admin', name: 'Thomas' });
    const ids = body<Array<{ camId: string }>>(await asAdmin('GET', '/v1/cameras')).map(
      (c) => c.camId,
    );
    expect(ids.sort()).toEqual([CAM_ID, 'CAM_OTHER'].sort());
    expect((await asAdmin('GET', `/v1/cameras/${CAM_ID}/media`)).statusCode).toBe(200);

    // A user without the admin role still sees only his own hub.
    expect(body<{ role: string }>(await world.call('GET', '/v1/me')).role).toBe('user');
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
