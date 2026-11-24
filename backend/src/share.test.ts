import { describe, expect, it } from 'vitest';
import { handleRequest } from './api';
import type { ApiRequest, ApiResponse } from './http';
import type { CheckinRequest } from './model';
import { MAX_SHARES_PER_USER } from './share';
import { CAM_ID, NOW, USER_ID, appRequest, hubRequest, setup } from './test-utils';

const json = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

const checkin: CheckinRequest = {
  cameras: [
    {
      camId: CAM_ID,
      readAt: NOW - 60,
      media: [
        { mediaId: 2035, dir: 100, num: 35, fileType: 2, mediaTime: 1_795_000_000 },
        { mediaId: 2037, dir: 100, num: 37, fileType: 0, mediaTime: 1_795_000_300 },
      ],
    },
  ],
};

/** A camera with a video (2035, in the cloud) and a picture (2037, thumbnail only). */
async function world() {
  const w = await setup();
  await handleRequest(hubRequest('/v1/device/checkin', w.hubKey, checkin), w.deps);
  await w.store.markThumbPresent(CAM_ID, 2035);
  await w.store.markThumbPresent(CAM_ID, 2037);
  await w.store.setVideoState(CAM_ID, 2035, 'available');
  const call = (method: string, path: string, data?: unknown, host = 'api.test') => {
    const request: ApiRequest = appRequest(method, path, data);
    request.headers.host = host;
    return handleRequest(request, w.deps);
  };
  const open = (path: string): Promise<ApiResponse> =>
    handleRequest({ method: 'GET', path, headers: {} }, w.deps);
  const share = async (mediaId: number, data: object) => {
    const response = await call('POST', `/v1/cameras/${CAM_ID}/media/${mediaId}/share`, data);
    return {
      response,
      link: response.statusCode === 200 ? json<{ url: string; id: string }>(response) : undefined,
    };
  };
  return { ...w, call, open, share };
}

const pathOf = (url: string) => new URL(url).pathname;

describe('share a recording with a link (#444)', () => {
  it('creates a link that opens without sign-in and shows only the recording', async () => {
    const { share, open } = await world();
    const { link } = await share(2037, { days: 7, kind: 'thumb' });
    expect(link?.url).toMatch(/^https:\/\/api\.test\/v1\/s\/[A-Za-z0-9_-]{22}$/);
    const page = await open(pathOf(link?.url ?? ''));
    expect(page.statusCode).toBe(200);
    expect(page.headers['content-type']).toContain('text/html');
    expect(page.headers['cache-control']).toBe('private, no-store');
    expect(page.headers['referrer-policy']).toBe('no-referrer');
    expect(page.headers['x-robots-tag']).toContain('noindex');
    expect(page.headers['content-security-policy']).toContain("default-src 'none'");
    expect(page.body).toContain('https://media.test/get/F4C15606ED3B/2037.jpg');
    // The camera's wall clock, read as UTC; no camera or hub name by default.
    expect(page.body).toContain('18.11.2026');
    expect(page.body).not.toContain('Garden hub');
  });

  it('shows a video, and the camera name only when asked', async () => {
    const { share, open, call } = await world();
    await call('POST', `/v1/cameras/${CAM_ID}/name`, { name: 'Waldrand & Nord' });
    const plain = await share(2035, { days: 1, kind: 'video' });
    const named = await share(2035, { days: 1, kind: 'video', showCamera: true });
    const first = await open(pathOf(plain.link?.url ?? ''));
    expect(first.body).toContain('<video src="https://media.test/get/F4C15606ED3B/2035.mp4"');
    expect(first.body).not.toContain('Waldrand');
    const second = await open(pathOf(named.link?.url ?? ''));
    expect(second.body).toContain('Waldrand &#38; Nord');
  });

  it('rejects wrong days, kind and recordings that are not in the cloud', async () => {
    const { share } = await world();
    expect((await share(2037, { days: 3, kind: 'thumb' })).response.statusCode).toBe(400);
    expect((await share(2037, { days: 7, kind: 'photo' })).response.statusCode).toBe(400);
    // 2037 has no video, 9999 does not exist.
    expect((await share(2037, { days: 7, kind: 'video' })).response.statusCode).toBe(409);
    expect((await share(9999, { days: 7, kind: 'thumb' })).response.statusCode).toBe(404);
  });

  it('is only for users of the hub', async () => {
    const { deps } = await world();
    const stranger = appRequest(
      'POST',
      `/v1/cameras/${CAM_ID}/media/2037/share`,
      { days: 1, kind: 'thumb' },
      `${USER_ID + 1}.test-secret-0123456789abcdef`,
    );
    expect((await handleRequest(stranger, deps)).statusCode).toBe(404);
    const anonymous: ApiRequest = {
      method: 'POST',
      path: `/v1/cameras/${CAM_ID}/media/2037/share`,
      headers: {},
      body: JSON.stringify({ days: 1, kind: 'thumb' }),
    };
    expect((await handleRequest(anonymous, deps)).statusCode).toBe(401);
  });

  it('answers 410 for unknown and malformed tokens alike', async () => {
    const { open } = await world();
    expect((await open('/v1/s/AAAAAAAAAAAAAAAAAAAAAA')).statusCode).toBe(410);
    expect((await open('/v1/s/short')).statusCode).toBe(410);
  });

  it('ends at the expiry time', async () => {
    const w = await world();
    const { link } = await w.share(2037, { days: 1, kind: 'thumb' });
    const path = pathOf(link?.url ?? '');
    expect((await w.open(path)).statusCode).toBe(200);
    const later = { ...w.deps, now: () => NOW + 24 * 60 * 60 };
    const response = await handleRequest({ method: 'GET', path, headers: {} }, later);
    expect(response.statusCode).toBe(410);
  });

  it('ends when the recording is gone from the backend', async () => {
    const w = await world();
    const { link } = await w.share(2037, { days: 7, kind: 'thumb' });
    await w.store.removeMedia(CAM_ID, [2037]);
    expect((await w.open(pathOf(link?.url ?? ''))).statusCode).toBe(410);
  });

  it('lists the own links and revokes one', async () => {
    const w = await world();
    const a = await w.share(2037, { days: 7, kind: 'thumb' });
    const b = await w.share(2035, { days: 1, kind: 'video' });
    const listed = json<Array<{ id: string; kind: string }>>(await w.call('GET', '/v1/shares'));
    expect(listed.map((s) => s.id).sort()).toEqual([a.link?.id, b.link?.id].sort());
    expect(JSON.stringify(listed)).not.toContain('/v1/s/');

    expect((await w.call('POST', `/v1/shares/${a.link?.id}/revoke`)).statusCode).toBe(200);
    expect((await w.open(pathOf(a.link?.url ?? ''))).statusCode).toBe(410);
    expect((await w.open(pathOf(b.link?.url ?? ''))).statusCode).toBe(200);
    expect(json<unknown[]>(await w.call('GET', '/v1/shares'))).toHaveLength(1);
    expect((await w.call('POST', `/v1/shares/${a.link?.id}/revoke`)).statusCode).toBe(404);
  });

  it('does not let one user revoke the links of another', async () => {
    const w = await world();
    const { link } = await w.share(2037, { days: 7, kind: 'thumb' });
    const other = appRequest(
      'POST',
      `/v1/shares/${link?.id}/revoke`,
      undefined,
      `${USER_ID + 1}.test-secret-0123456789abcdef`,
    );
    expect((await handleRequest(other, w.deps)).statusCode).toBe(404);
    expect((await w.open(pathOf(link?.url ?? ''))).statusCode).toBe(200);
  });

  it('limits the active links per user, expired ones do not count', async () => {
    const w = await world();
    for (let i = 0; i < MAX_SHARES_PER_USER; i++) {
      expect((await w.share(2037, { days: 1, kind: 'thumb' })).response.statusCode).toBe(200);
    }
    expect((await w.share(2037, { days: 1, kind: 'thumb' })).response.statusCode).toBe(429);
    const later = { ...w.deps, now: () => NOW + 2 * 24 * 60 * 60 };
    const request = appRequest('POST', `/v1/cameras/${CAM_ID}/media/2037/share`, {
      days: 1,
      kind: 'thumb',
    });
    expect((await handleRequest(request, later)).statusCode).toBe(200);
  });

  it('writes the audit log', async () => {
    const w = await world();
    const { link } = await w.share(2037, { days: 7, kind: 'thumb' });
    await w.call('POST', `/v1/shares/${link?.id}/revoke`);
    const log = await w.store.listAudit(10);
    expect(log.map((e) => e.action).sort()).toEqual(['shareCreated', 'shareRevoked']);
    expect(log.every((e) => e.actor === USER_ID)).toBe(true);
  });
});
