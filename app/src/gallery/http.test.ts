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

  it('shows a video and its photo as one recording with both file names', async () => {
    // From a real TC100 list (newest first): the photo has the next number
    // and is up to 12 s older than the video's time.
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/cameras': [camera],
      'GET https://api/v1/cameras/CAM1/media': [
        item({ mediaId: 33, num: 33, fileType: 2, mediaTime: 1794731054 }),
        item({
          mediaId: 34,
          num: 34,
          fileType: 0,
          mediaTime: 1794731042,
          thumbUrl: 'https://s3/photo',
        }),
        item({ mediaId: 28, num: 28, fileType: 0, mediaTime: 1794661708 }),
        item({ mediaId: 27, num: 27, fileType: 2, mediaTime: 1794661708, videoState: 'failed' }),
        item({ mediaId: 25, num: 25, fileType: 2, mediaTime: 1794658538, videoState: 'requested' }),
        item({ mediaId: 26, num: 26, fileType: 0, mediaTime: 1794658526 }),
        item({ mediaId: 24, num: 24, fileType: 0, mediaTime: 1794656736 }),
        item({ mediaId: 23, num: 23, fileType: 2, mediaTime: 1794656736 }),
        item({ mediaId: 22, num: 22, fileType: 0, mediaTime: 1794642848 }),
      ],
      'POST https://api/v1/cameras/CAM1/media/25/withdraw': { withdrawn: 1 },
      'POST https://api/v1/cameras/CAM1/media/26/withdraw': { withdrawn: 0 },
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.status();
    const list = await api.recordings('CAM1');
    expect(list.map((r) => r.cameraFiles)).toEqual([
      ['DSCF0034.JPG', 'DSCF0033.AVI'],
      ['DSCF0028.JPG', 'DSCF0027.AVI'],
      ['DSCF0026.JPG', 'DSCF0025.AVI'],
      ['DSCF0024.JPG', 'DSCF0023.AVI'],
      ['DSCF0022.JPG'],
    ]);
    expect(list[0]).toMatchObject({
      mediaId: 33,
      mediaIds: [34, 33],
      thumbUrl: 'https://s3/photo',
    });
    expect(list[2]).toMatchObject({ mediaId: 25, state: 'videoRequested', fileType: 2 });
    expect(list[4]).toMatchObject({ fileType: 0, mediaIds: [22] });

    await api.withdraw('CAM1', list[2]!);
    expect(calls.slice(-2).map((c) => c.url)).toEqual([
      'https://api/v1/cameras/CAM1/media/26/withdraw',
      'https://api/v1/cameras/CAM1/media/25/withdraw',
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

  it('splits a delete of more than 200 files', async () => {
    const { fn, calls } = fakeFetch({ 'POST https://api/v1/cameras/CAM1/commands': [] });
    const ids = Array.from({ length: 450 }, (_, i) => i + 1);
    await new HttpGalleryApi('https://api', 't', fn).requestDelete('CAM1', ids);
    const sent = calls.map(
      (c) => (JSON.parse(String(c.init?.body)) as { mediaIds: number[] }).mediaIds,
    );
    expect(sent.map((list) => list.length)).toEqual([200, 200, 50]);
    expect(sent.flat()).toEqual(ids);
  });

  it('renames a camera and uses the new name for the file names', async () => {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/cameras': [camera],
      'POST https://api/v1/cameras/CAM1/name': { camId: 'CAM1', name: 'Futterplatz' },
      'GET https://api/v1/cameras/CAM1/media': [item({ mediaId: 1, num: 1 })],
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.status();
    expect(await api.renameCamera('CAM1', ' Futterplatz ')).toBe('Futterplatz');
    expect(JSON.parse(String(calls[1]?.init?.body))).toEqual({ name: ' Futterplatz ' });
    const [recording] = await api.recordings('CAM1');
    expect(recording?.fileName).toMatch(/^Futterplatz_/);
  });
});

describe('pairing trace', () => {
  it('says why a video stays alone', async () => {
    const { groupRecordings } = await import('./http');
    const rows: Record<string, unknown>[] = [];
    groupRecordings(
      [
        item({ mediaId: 1, num: 1, fileType: 2, mediaTime: 1_000 }),
        item({ mediaId: 2, num: 2, fileType: 0, mediaTime: 2_000 }),
      ],
      (r) => rows.push(...r),
    );
    expect(rows.map((r) => r.result)).toEqual(['too far apart', 'no photo']);
  });
});

describe('pairing with a missing photo', () => {
  it('does not let a video take the photo of the next pair', async () => {
    const { groupRecordings } = await import('./http');
    const t = 1_794_731_000;
    // Video 35 lost its photo 36 (deleted first); pair 33/34 was taken
    // a minute earlier. Newest first, as the backend lists them.
    const groups = groupRecordings([
      item({ mediaId: 35, num: 35, fileType: 2, mediaTime: t + 60, deleteState: 'delivered' }),
      item({ mediaId: 33, num: 33, fileType: 2, mediaTime: t + 12, deleteState: 'requested' }),
      item({ mediaId: 34, num: 34, fileType: 0, mediaTime: t, deleteState: 'requested' }),
    ]);
    expect(groups.map((g) => g.map((m) => m.num))).toEqual([[35], [34, 33]]);
  });
});
