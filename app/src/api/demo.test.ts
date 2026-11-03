import { describe, expect, it } from 'vitest';
import { buildDemoMedia, DemoApiClient } from './demo';

describe('buildDemoMedia', () => {
  it('returns photo/video pairs, newest first', () => {
    const media = buildDemoMedia(1_000_000, 4);
    expect(media.map((m) => m.fileType)).toEqual([0, 2, 0, 2]);
    expect(media.map((m) => m.num)).toEqual([36, 35, 34, 33]);
    expect(new Set(media.map((m) => m.mediaId)).size).toBe(4);
    const times = media.map((m) => m.mediaTime);
    expect([...times].sort((a, b) => b - a)).toEqual(times);
  });

  it('uses inline SVG thumbnails', () => {
    expect(buildDemoMedia(0, 1)[0]?.thumbUrl).toMatch(/^data:image\/svg\+xml/);
  });
});

describe('DemoApiClient', () => {
  const client = () => new DemoApiClient(() => 1_000_000, 0);

  it('lists two demo cameras on one hub', async () => {
    const cameras = await client().listCameras();
    expect(cameras.map((c) => c.name)).toEqual(['Garden', 'Meadow']);
    expect(new Set(cameras.map((c) => c.hubId)).size).toBe(1);
    expect(cameras[0]?.lastSeen).toBe(1_000_000 - 17 * 60);
  });

  it('keeps media per camera', async () => {
    const api = client();
    const [garden, meadow] = await api.listCameras();
    const gardenMedia = await api.listMedia(garden?.camId ?? '');
    const meadowMedia = await api.listMedia(meadow?.camId ?? '');
    expect(gardenMedia[0]?.num).toBe(36);
    expect(meadowMedia[0]?.num).toBe(12);
    expect(await api.listMedia('unknown')).toEqual([]);
  });

  it('marks a requested video and keeps it after reloading the list', async () => {
    const api = client();
    const [camera] = await api.listCameras();
    const camId = camera?.camId ?? '';
    const video = (await api.listMedia(camId)).find(
      (m) => m.fileType === 2 && m.videoState === 'none',
    );
    expect(video).toBeDefined();

    const result = await api.requestVideo(camId, video!);

    expect(result).toMatchObject({ type: 'requestVideo', state: 'queued' });
    const reloaded = (await api.listMedia(camId)).find((m) => m.mediaId === video!.mediaId);
    expect(reloaded?.videoState).toBe('requested');
  });

  it('rejects unknown media', async () => {
    await expect(client().requestVideo('nope', { mediaId: 1 })).rejects.toThrow('Unknown media');
  });
});
