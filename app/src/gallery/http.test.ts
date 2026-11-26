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

  it('sends the targets of a delete only when the user chose them (#461)', async () => {
    const { fn, calls } = fakeFetch({ 'POST https://api/v1/cameras/CAM1/commands': [] });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.requestDelete('CAM1', [1], ['camera', 'backend']);
    expect(JSON.parse(String(calls[0]?.init?.body))).toEqual({
      type: 'deleteMedia',
      mediaIds: [1],
      targets: ['camera', 'backend'],
    });
  });

  it("saves the hub's delete targets and reads them from the cameras (#461)", async () => {
    const { fn, calls } = fakeFetch({
      'POST https://api/v1/hubs/h1/delete-targets': { targets: ['hub'] },
      'GET https://api/v1/cameras': [
        { ...camera, hubId: 'h1', hubDeleteTargets: ['hub', 'backend'], hubDeletesByTarget: true },
      ],
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.setDeleteTargets('h1', ['hub']);
    expect(JSON.parse(String(calls[0]?.init?.body))).toEqual({ targets: ['hub'] });
    const hub = (await api.status()).hub;
    expect(hub?.deleteTargets).toEqual(['hub', 'backend']);
    expect(hub?.deletesByTarget).toBe(true);
  });

  it('treats a backend without the setting as camera and gallery (#461)', async () => {
    const { fn } = fakeFetch({ 'GET https://api/v1/cameras': [{ ...camera, hubId: 'h1' }] });
    const hub = (await new HttpGalleryApi('https://api', 't', fn).status()).hub;
    expect(hub?.deleteTargets).toEqual(['camera', 'backend']);
    expect(hub?.deletesByTarget).toBe(false);
  });

  it('marks a recording that is gone from the camera only (#461)', async () => {
    const { fn } = fakeFetch({
      'GET https://api/v1/cameras': [camera],
      'GET https://api/v1/cameras/CAM1/media': [
        item({ mediaId: 1, num: 1, onCamera: false, videoState: 'available', videoUrl: 'u' }),
        item({ mediaId: 5, num: 5, dir: 101 }),
      ],
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    await api.status();
    const list = await api.recordings('CAM1');
    expect(list.map((r) => r.goneFromCamera ?? false)).toEqual([true, false]);
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

  it('asks who is signed in and calls the admin endpoints', async () => {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/me': { userId: 1, name: 'Thomas', role: 'admin' },
      'GET https://api/v1/admin/hubs': [{ hubId: 'h1' }],
      'POST https://api/v1/hubs/h1/schedule': {},
      'POST https://api/v1/cameras/CAM1/commands': {},
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    expect(await api.me()).toEqual({ name: 'Thomas', admin: true });
    expect((await api.adminHubs()).map((h) => h.hubId)).toEqual(['h1']);
    await api.setSchedule('h1', { checkinIntervalS: 3600 });
    expect(JSON.parse(String(calls[2]?.init?.body))).toEqual({
      cameraPlan: null,
      checkinIntervalS: 3600,
      powerSetting: null,
    });
    await api.setSchedule('h1', { powerSetting: 'battery' });
    expect(JSON.parse(String(calls[3]?.init?.body))).toEqual({
      cameraPlan: null,
      checkinIntervalS: null,
      powerSetting: 'battery',
    });
    await api.readCamera('CAM1');
    expect(JSON.parse(String(calls[4]?.init?.body))).toEqual({ type: 'readCamera' });
  });

  it('lists the cameras every hub found and sends a decision (#298)', async () => {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/me': { name: 'Opa', role: 'user', hubs: [{ hubId: 'h1' }] },
      'GET https://api/v1/hubs/h1/found-cameras': [
        { bleAddress: 'AA:BB:CC:00:00:01', state: 'new', name: 'TC100_0001', lastSeen: 5 },
      ],
      'POST https://api/v1/hubs/h1/found-cameras/AA%3ABB%3ACC%3A00%3A00%3A01': {},
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    expect(await api.foundCameras()).toEqual([
      {
        hubId: 'h1',
        bleAddress: 'AA:BB:CC:00:00:01',
        state: 'new',
        name: 'TC100_0001',
        lastSeen: 5,
      },
    ]);
    await api.decideCamera('h1', 'AA:BB:CC:00:00:01', 'approved', 'Futterplatz');
    expect(JSON.parse(String(calls[2]?.init?.body))).toEqual({
      state: 'approved',
      name: 'Futterplatz',
    });
  });

  it('asks every hub for a scan (#308)', async () => {
    const { fn } = fakeFetch({
      'GET https://api/v1/me': {
        name: 'Opa',
        role: 'user',
        hubs: [{ hubId: 'h1' }, { hubId: 'h2' }],
      },
      'POST https://api/v1/hubs/h1/scan': { requested: 2, nextCheckinAt: 500 },
      'POST https://api/v1/hubs/h2/scan': { requested: 0 },
    });
    const api = new HttpGalleryApi('https://api', 't', fn);
    expect(await api.requestScan()).toEqual({ requested: 2, nextContact: 500 });
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

describe('pairing switched off (#347)', () => {
  it('shows every file alone but still works out and logs the pairs', async () => {
    const { groupRecordings } = await import('./http');
    const t = 1_794_731_000;
    const items = [
      item({ mediaId: 42, num: 42, fileType: 2, mediaTime: t + 12 }),
      item({ mediaId: 43, num: 43, fileType: 0, mediaTime: t }),
    ];
    const rows: Record<string, unknown>[] = [];
    const alone = groupRecordings(items, (r) => rows.push(...r), false);
    expect(alone.map((g) => g.map((m) => m.num))).toEqual([[42], [43]]);
    expect(rows.map((r) => r.result)).toEqual(['paired']);
    expect(groupRecordings(items).map((g) => g.map((m) => m.num))).toEqual([[43, 42]]);
  });
});

describe('debug switches only for the admin (#431)', () => {
  const t = 1_794_731_000;
  const media = [
    item({ mediaId: 42, num: 42, fileType: 2, mediaTime: t + 12 }),
    item({ mediaId: 43, num: 43, fileType: 0, mediaTime: t }),
  ];

  async function tiles(role: string, pairing: boolean) {
    const { fn, calls } = fakeFetch({
      'GET https://api/v1/cameras/CAM1/media': media,
      'GET https://api/v1/me': { name: 'X', role },
    });
    const api = new HttpGalleryApi('https://api', 't', fn, false, undefined, undefined, pairing);
    const recordings = await api.recordings('CAM1');
    return {
      count: recordings.length,
      asked: calls.filter((c) => c.url.endsWith('/v1/me')).length,
    };
  }

  it('ignores ?pairing=off for a user', async () => {
    expect(await tiles('user', false)).toEqual({ count: 1, asked: 1 });
  });

  it('applies ?pairing=off for the admin', async () => {
    expect(await tiles('admin', false)).toEqual({ count: 2, asked: 1 });
  });

  it('does not ask who the user is without a switch', async () => {
    expect(await tiles('admin', true)).toEqual({ count: 1, asked: 0 });
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

describe('admin session (#259)', () => {
  /** A backend that accepts the session, or answers it with 401 (ended early). */
  function sessionFetch(options: { rejectSession?: boolean } = {}) {
    const seen: Array<{ path: string; authorization?: string }> = [];
    const fn = (async (url: string, init?: RequestInit) => {
      const path = url.replace('https://api', '');
      const authorization = (init?.headers as Record<string, string>).Authorization;
      seen.push({ path, authorization });
      if (path === '/v1/admin/elevate') {
        return new Response(JSON.stringify({ session: 'SESSION', expiresAt: 1_900 }), {
          status: 200,
        });
      }
      if (authorization?.startsWith('Admin ') && options.rejectSession) {
        return new Response('{"error":"expired"}', { status: 401 });
      }
      return new Response('[]', { status: 200 });
    }) as unknown as typeof fetch;
    return { fn, seen };
  }

  it('sends the code with the plain token, then uses the session until it expires', async () => {
    const { fn, seen } = sessionFetch();
    let now = 1_000;
    const api = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => now);
    expect(api.elevated()).toBe(false);
    await api.elevate('123456');
    expect(api.elevated()).toBe(true);
    await api.adminHubs();
    now = 1_900;
    expect(api.elevated()).toBe(false);
    await api.adminAudit();
    expect(seen.map((s) => s.authorization)).toEqual([
      'Bearer TOKEN',
      'Admin SESSION',
      'Bearer TOKEN',
    ]);
  });

  /** A tiny stand-in for `sessionStorage`. */
  function memoryStorage(initial: Record<string, string> = {}) {
    const data = new Map(Object.entries(initial));
    return {
      data,
      getItem: (key: string) => data.get(key) ?? null,
      setItem: (key: string, value: string) => void data.set(key, value),
      removeItem: (key: string) => void data.delete(key),
    };
  }

  it('keeps the session over a reload for the same token (#345)', async () => {
    const storage = memoryStorage();
    const first = sessionFetch();
    const before = new HttpGalleryApi(
      'https://api',
      'TOKEN',
      first.fn,
      false,
      () => 1_000,
      storage,
    );
    await before.elevate('123456');

    // A reload: a new object with the same storage.
    const second = sessionFetch();
    const after = new HttpGalleryApi(
      'https://api',
      'TOKEN',
      second.fn,
      false,
      () => 1_100,
      storage,
    );
    expect(after.elevated()).toBe(true);
    await after.adminHubs();
    expect(second.seen.map((s) => s.authorization)).toEqual(['Admin SESSION']);
  });

  it('does not restore an expired session or one of another token (#345)', async () => {
    const storage = memoryStorage();
    const { fn } = sessionFetch();
    await new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000, storage).elevate('1');

    const expired = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_900, storage);
    expect(expired.elevated()).toBe(false);
    expect(storage.data.size).toBe(0);

    await new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000, storage).elevate('1');
    const other = new HttpGalleryApi('https://api', 'OTHER', fn, false, () => 1_100, storage);
    expect(other.elevated()).toBe(false);
  });

  it('forgets the stored session when the backend ends it (#345)', async () => {
    const storage = memoryStorage();
    const { fn } = sessionFetch({ rejectSession: true });
    const api = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000, storage);
    await api.elevate('123456');
    expect(storage.data.size).toBe(1);
    await api.status();
    expect(storage.data.size).toBe(0);
  });

  it('works when the storage throws or holds garbage (#345)', async () => {
    const broken = {
      getItem: () => {
        throw new Error('blocked');
      },
      setItem: () => {
        throw new Error('blocked');
      },
      removeItem: () => {
        throw new Error('blocked');
      },
    };
    const { fn } = sessionFetch();
    const api = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000, broken);
    await api.elevate('123456');
    expect(api.elevated()).toBe(true);

    const garbage = memoryStorage({ 'trailcamhub.adminSession': '{nope' });
    const other = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000, garbage);
    expect(other.elevated()).toBe(false);
  });

  it('falls back to the token when the backend ends the session', async () => {
    const { fn, seen } = sessionFetch({ rejectSession: true });
    const api = new HttpGalleryApi('https://api', 'TOKEN', fn, false, () => 1_000);
    await api.elevate('123456');
    await api.status();
    expect(api.elevated()).toBe(false);
    expect(seen.map((s) => s.authorization)).toEqual([
      'Bearer TOKEN',
      'Admin SESSION',
      'Bearer TOKEN',
    ]);
  });
});
