import { describe, expect, it } from 'vitest';
import type { Camera, MediaItem } from '../api/types';
import { HttpGalleryApi } from './http';

const camera = {
  camId: 'CAM1',
  name: 'Garten',
  hubLastSeen: 1_000,
  hubBatPercent: 80,
  hubSchedule: { nextCheckinAt: 2_800 },
} as unknown as Camera;

const item = (over: Partial<MediaItem>): MediaItem => ({
  mediaId: 1,
  dir: 100,
  num: 1,
  fileType: 2,
  mediaTime: Date.UTC(2026, 10, 15, 14, 2) / 1000,
  thumbUrl: 'https://s3/thumb',
  videoState: 'none',
  ...over,
});

function fakeFetch(routes: Record<string, unknown>) {
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fn = (async (url: string, init?: RequestInit) => {
    calls.push({ url, init });
    const key = `${init?.method ?? 'GET'} ${url}`;
    if (!(key in routes)) return new Response('{"error":"nope"}', { status: 404 });
    return new Response(JSON.stringify(routes[key]), { status: 200 });
  }) as unknown as typeof fetch;
  return { fn, calls };
}

describe('HttpGalleryApi', () => {
  it('sends the app token and maps the backend states', async () => {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/cameras': [camera],
      'GET https://api/v1/cameras/CAM1/media': [
        item({ mediaId: 1, num: 1 }),
        item({ mediaId: 2, num: 2, videoState: 'downloading' }),
        item({ mediaId: 3, num: 3, videoState: 'available', videoUrl: 'https://s3/v.mp4' }),
        item({ mediaId: 4, num: 4, deleteState: 'delivered' }),
        item({
          mediaId: 5,
          num: 5,
          deleteState: 'failed',
          deleteError: 'unsupported command deleteMedia',
        }),
      ],
    });
    const api = new HttpGalleryApi('https://api/', '1000000000000001.secretsecretsecret', fn);

    const status = await api.status();
    expect(status).toEqual({
      lastContact: 1_000,
      nextContact: 2_800,
      hubBatPercent: 80,
      cameras: [{ camId: 'CAM1', name: 'Garten' }],
    });
    const list = await api.recordings('CAM1');
    expect(list.map((r) => r.state)).toEqual([
      'new',
      'videoRequested',
      'videoReady',
      'deleteRequested',
      'failed',
    ]);
    expect(list[4]?.failReason).toMatch(/neue Firmware/);
    expect(list[0]?.fileName).toBe('Garten_2026-11-15_14-02_DSCF0001.mp4');
    expect(new Headers(calls[0]?.init?.headers).get('Authorization')).toBe(
      'Bearer 1000000000000001.secretsecretsecret',
    );
  });

  it('shows the photo and the video of one camera file as one recording', async () => {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/cameras': [camera],
      'GET https://api/v1/cameras/CAM1/media': [
        item({ mediaId: 11, num: 12, fileType: 0, thumbUrl: 'https://s3/photo' }),
        item({ mediaId: 12, num: 12, fileType: 2, videoState: 'requested' }),
        item({ mediaId: 13, num: 13, fileType: 0 }),
      ],
      'POST https://api/v1/cameras/CAM1/media/11/withdraw': { withdrawn: 0 },
      'POST https://api/v1/cameras/CAM1/media/12/withdraw': { withdrawn: 1 },
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.status();
    const list = await api.recordings('CAM1');
    expect(list).toHaveLength(2);
    expect(list[0]).toMatchObject({
      mediaId: 12,
      mediaIds: [11, 12],
      cameraFile: 'DSCF0012',
      suffixes: ['JPG', 'AVI'],
      fileType: 2,
      state: 'videoRequested',
      thumbUrl: 'https://s3/photo',
    });
    expect(list[1]).toMatchObject({ cameraFile: 'DSCF0013', suffixes: ['JPG'], fileType: 0 });

    await api.withdraw('CAM1', list[0]!);
    expect(calls.slice(-2).map((c) => c.url)).toEqual([
      'https://api/v1/cameras/CAM1/media/11/withdraw',
      'https://api/v1/cameras/CAM1/media/12/withdraw',
    ]);
  });

  it('sends a bulk delete as one request', async () => {
    const { fn, calls } = fakeFetch({ 'POST https://api/v1/cameras/CAM1/commands': [] });
    await new HttpGalleryApi('https://api', 't', fn).requestDelete('CAM1', [1, 2, 3]);
    expect(calls).toHaveLength(1);
    expect(JSON.parse(String(calls[0]?.init?.body))).toEqual({
      type: 'deleteMedia',
      mediaIds: [1, 2, 3],
    });
  });
});
