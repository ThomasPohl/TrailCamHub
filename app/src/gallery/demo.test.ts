import { describe, expect, it } from 'vitest';
import { DemoGalleryApi } from './demo';

describe('DemoGalleryApi', () => {
  it('delivers requested videos and deletes marked recordings at the next check-in', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    const { cameras } = await api.status();
    const camId = cameras[0]!.camId;
    const [first, second] = await api.recordings(camId);

    await api.requestVideos(camId, [first!.mediaId]);
    await api.requestDelete(camId, [second!.mediaId]);
    let list = await api.recordings(camId);
    expect(list.find((r) => r.mediaId === first!.mediaId)?.state).toBe('videoRequested');
    expect(list.find((r) => r.mediaId === second!.mediaId)?.state).toBe('deleteRequested');

    now += 20;
    list = await api.recordings(camId);
    expect(list.find((r) => r.mediaId === first!.mediaId)?.state).toBe('videoReady');
    expect(list.some((r) => r.mediaId === second!.mediaId)).toBe(false);
    expect((await api.status()).lastContact).toBe(now);
  });

  it('withdraws a request before the hub picks it up', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    const camId = (await api.status()).cameras[0]!.camId;
    const [first] = await api.recordings(camId);

    await api.requestDelete(camId, [first!.mediaId]);
    await api.withdraw(camId, first!.mediaId);
    expect((await api.recordings(camId))[0]?.state).toBe('new');
  });
});
