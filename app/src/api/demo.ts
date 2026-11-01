import type { ApiClient, Camera, CommandResult, MediaItem } from './types';
import { toCameraWallClock } from '../format';

const DEMO_HUB = { hubId: 'garden-hub', hubName: 'Garden hub', hubBatPercent: 91 };

const DEMO_CAMERAS: Array<Camera & { firstNum: number; minutesAgo: number }> = [
  {
    ...DEMO_HUB,
    camId: 'F4C15606ED3B',
    name: 'Garden',
    model: 'TC100',
    firmware: '2.3.0.3',
    batPercent: 78,
    sdFreeMB: 28_412,
    lastSeen: 0,
    firstNum: 36,
    minutesAgo: 17,
  },
  {
    ...DEMO_HUB,
    camId: 'A1B2C3D4E5F6',
    name: 'Meadow',
    model: 'TC100',
    firmware: '2.3.0.3',
    batPercent: 22,
    sdFreeMB: 9_870,
    lastSeen: 0,
    firstNum: 12,
    minutesAgo: 19,
  },
];

/** Night-vision style placeholder thumbnail, so the demo needs no binary assets. */
export function placeholderThumb(label: string, hue: number): string {
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 120">` +
    `<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">` +
    `<stop offset="0" stop-color="hsl(${hue},18%,32%)"/>` +
    `<stop offset="1" stop-color="hsl(${hue},22%,14%)"/></linearGradient></defs>` +
    `<rect width="160" height="120" fill="url(#g)"/>` +
    `<path d="M0 92 Q40 70 80 88 T160 80 V120 H0Z" fill="hsl(${hue},20%,10%)"/>` +
    `<text x="8" y="112" font-family="monospace" font-size="11" fill="#ffffffb0">${label}</text>` +
    `</svg>`;
  return `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
}

/** Photo + video pairs, newest first, as the TC100 reports them (DCIM 100). */
export function buildDemoMedia(now: number, count = 12, firstNum = 36): MediaItem[] {
  const items: MediaItem[] = [];
  for (let i = 0; i < count; i++) {
    const num = firstNum - i;
    // Like the camera: the wall clock, not the UTC instant.
    const mediaTime = toCameraWallClock(now - i * 5_400 - 600);
    const isVideo = i % 2 === 1;
    items.push({
      dir: 100,
      num,
      fileType: isVideo ? 2 : 0,
      mediaTime,
      thumbUrl: placeholderThumb(`100_${num}`, (num * 37) % 360),
      videoState: isVideo && i === 1 ? 'available' : 'none',
    });
  }
  return items;
}

/**
 * In-memory stand-in for the backend. Used when no API URL is configured,
 * so the app can be developed and demonstrated before the AWS side exists.
 */
export class DemoApiClient implements ApiClient {
  readonly isDemo = true;
  private readonly media = new Map<string, MediaItem[]>();
  private nextCommandId = 1;

  constructor(
    private readonly now: () => number = () => Math.floor(Date.now() / 1000),
    private readonly latencyMs = 250,
  ) {
    for (const camera of DEMO_CAMERAS) {
      this.media.set(camera.camId, buildDemoMedia(this.now(), 12, camera.firstNum));
    }
  }

  async listCameras(): Promise<Camera[]> {
    await this.delay();
    return DEMO_CAMERAS.map(({ firstNum: _firstNum, minutesAgo, ...camera }) => {
      void _firstNum;
      return { ...camera, lastSeen: this.now() - minutesAgo * 60 };
    });
  }

  async listMedia(camId: string): Promise<MediaItem[]> {
    await this.delay();
    return (this.media.get(camId) ?? []).map((m) => ({ ...m }));
  }

  async requestVideo(camId: string, item: Pick<MediaItem, 'dir' | 'num'>): Promise<CommandResult> {
    await this.delay();
    const target = this.media.get(camId)?.find((m) => m.dir === item.dir && m.num === item.num);
    if (!target) {
      throw new Error(`Unknown media ${item.dir}_${item.num}`);
    }
    if (target.videoState === 'none' || target.videoState === 'failed') {
      target.videoState = 'requested';
    }
    return { id: `demo-${this.nextCommandId++}`, type: 'requestVideo', state: 'queued' };
  }

  private delay(): Promise<void> {
    return this.latencyMs > 0
      ? new Promise((resolve) => setTimeout(resolve, this.latencyMs))
      : Promise.resolve();
  }
}
