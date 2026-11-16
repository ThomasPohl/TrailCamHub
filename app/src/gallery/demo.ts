import { placeholderThumb } from '../api/demo';
import { toCameraWallClock } from '../format';
import type { GalleryApi, GalleryCamera, GalleryStatus, Recording } from './types';
import { cameraFileName, videoFileName } from './text';

const CAMERAS: GalleryCamera[] = [
  { camId: 'F4C15606ED3B', name: 'Garten' },
  { camId: 'A1B2C3D4E5F6', name: 'Wiese' },
];

/** How often the simulated hub "checks in" in the demo. */
export const DEMO_CHECKIN_S = 20;

function buildRecordings(camera: GalleryCamera, now: number, count: number): Recording[] {
  const list: Recording[] = [];
  for (let i = 0; i < count; i++) {
    const num = 60 - i;
    const mediaTime = toCameraWallClock(now - i * 2_700 - 900);
    const mediaId = 4_136_037_000 + num + (camera.camId === 'A1B2C3D4E5F6' ? 500 : 0);
    list.push({
      mediaId,
      // The camera keeps a photo and a video under the same name.
      mediaIds: [mediaId, mediaId + 100_000],
      camId: camera.camId,
      fileType: 2,
      cameraFile: cameraFileName(num),
      suffixes: ['JPG', 'AVI'],
      mediaTime,
      thumbUrl: placeholderThumb(`${camera.name} ${num}`, (num * 37) % 360),
      state: 'new',
      fileName: videoFileName(camera.name, mediaTime, num),
    });
  }
  return list;
}

/**
 * In-memory backend with a simulated hub that checks in every
 * DEMO_CHECKIN_S seconds: it then delivers requested videos and deletes what
 * was marked. Lets the gallery be tried before the backend part exists.
 */
export class DemoGalleryApi implements GalleryApi {
  readonly isDemo = true;
  private readonly media = new Map<string, Recording[]>();
  private lastCheckin: number;

  constructor(
    private readonly now: () => number = () => Math.floor(Date.now() / 1000),
    private readonly checkinS = DEMO_CHECKIN_S,
  ) {
    const start = this.now();
    this.lastCheckin = start;
    for (const camera of CAMERAS) this.media.set(camera.camId, buildRecordings(camera, start, 16));
  }

  async status(): Promise<GalleryStatus> {
    this.catchUp();
    return {
      lastContact: this.lastCheckin,
      nextContact: this.lastCheckin + this.checkinS,
      hubBatPercent: 84,
      cameras: CAMERAS.map((c) => ({ ...c })),
    };
  }

  async recordings(camId: string): Promise<Recording[]> {
    this.catchUp();
    return this.list(camId)
      .filter((r) => r.state !== 'deleted')
      .map((r) => ({ ...r }));
  }

  async requestVideos(camId: string, mediaIds: number[]): Promise<void> {
    this.catchUp();
    for (const r of this.pick(camId, mediaIds)) {
      if (r.state === 'new' || r.state === 'failed') {
        r.state = 'videoRequested';
        r.failReason = undefined;
      }
    }
  }

  async requestDelete(camId: string, mediaIds: number[]): Promise<void> {
    this.catchUp();
    for (const r of this.pick(camId, mediaIds)) {
      if (r.state !== 'deleted') r.state = 'deleteRequested';
    }
  }

  async withdraw(camId: string, recording: Recording): Promise<void> {
    this.catchUp();
    const [r] = this.pick(camId, [recording.mediaId]);
    if (r && (r.state === 'videoRequested' || r.state === 'deleteRequested')) {
      r.state = r.videoUrl ? 'videoReady' : 'new';
    }
  }

  async download(recording: Recording): Promise<Blob> {
    const text =
      `Demo-Datei statt eines Videos.\n` +
      `Aufnahme ${recording.cameraFile} (${recording.fileName}).\n` +
      `Mit echtem Hub liegt hier das MP4.\n`;
    return new Blob([text], { type: 'text/plain' });
  }

  /** Runs the check-ins that would have happened since the last call. */
  private catchUp(): void {
    const now = this.now();
    while (now >= this.lastCheckin + this.checkinS) {
      this.lastCheckin += this.checkinS;
      for (const list of this.media.values()) {
        for (const r of list) {
          if (r.state === 'deleteRequested') r.state = 'deleted';
          else if (r.state === 'videoRequested') {
            r.state = 'videoReady';
            r.videoUrl = `demo:${r.mediaId}`;
          }
        }
      }
    }
  }

  private list(camId: string): Recording[] {
    const list = this.media.get(camId);
    if (!list) throw new Error(`Unbekannte Kamera ${camId}`);
    return list;
  }

  private pick(camId: string, mediaIds: number[]): Recording[] {
    const ids = new Set(mediaIds);
    return this.list(camId).filter((r) => r.mediaIds.some((id) => ids.has(id)));
  }
}
