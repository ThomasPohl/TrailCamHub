import { describe, expect, it } from 'vitest';
import { createAppUser } from './admin';
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
