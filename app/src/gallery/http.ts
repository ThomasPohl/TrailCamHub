import { ApiError } from '../api/http';
import type { Camera, MediaItem } from '../api/types';
import { cameraFileName, cameraSuffix, videoFileName } from './text';
import type { GalleryApi, GalleryStatus, Recording, RecordingState } from './types';

/** A reason from the hub or backend in words the user can understand. */
export function friendlyReason(error: string | undefined): string | undefined {
  if (!error) return undefined;
  if (/unsupported command/i.test(error)) {
    return 'Der Hub kann das noch nicht (neue Firmware nötig).';
  }
  if (/no longer on the camera|not on the camera|unavailable/i.test(error)) {
    return 'Die Aufnahme ist nicht mehr auf der Kamera.';
  }
  if (/too large/i.test(error)) return 'Das Video ist zu groß für die Übertragung.';
  return `Grund: ${error}`;
}

function toState(m: MediaItem): Pick<Recording, 'state' | 'failReason'> {
  if (m.deleteState === 'requested' || m.deleteState === 'delivered') {
    return { state: 'deleteRequested' };
  }
  if (m.deleteState === 'failed') {
    return { state: 'failed', failReason: friendlyReason(m.deleteError) };
  }
  const byVideo: Record<MediaItem['videoState'], RecordingState> = {
    none: 'new',
    requested: 'videoRequested',
    downloading: 'videoRequested',
    available: 'videoReady',
    failed: 'failed',
  };
  const state = byVideo[m.videoState];
  return state === 'failed' ? { state, failReason: friendlyReason(m.videoError) } : { state };
}

/**
 * The files of one recording: the camera keeps a photo and a video under the
 * same name (dir/num), and the user sees them as one (#273). Newest first,
 * like the backend's list.
 */
export function groupRecordings(items: MediaItem[]): MediaItem[][] {
  const groups = new Map<string, MediaItem[]>();
  for (const m of items) {
    const key = `${m.dir}/${m.num}`;
    const group = groups.get(key);
    if (group) group.push(m);
    else groups.set(key, [m]);
  }
  return [...groups.values()];
}

/**
 * The gallery's backend: the TrailCamHub API v1 with an app token (#259),
 * `Authorization: Bearer <userId>.<secret>`.
 */
export class HttpGalleryApi implements GalleryApi {
  readonly isDemo = false;
  private readonly baseUrl: string;
  private cameras = new Map<string, Camera>();

  constructor(
    baseUrl: string,
    private readonly token: string,
    private readonly fetchFn: typeof fetch = (...args) => fetch(...args),
  ) {
    this.baseUrl = baseUrl.replace(/\/+$/, '');
  }

  async status(): Promise<GalleryStatus> {
    const cameras = await this.request<Camera[]>('GET', '/v1/cameras');
    this.cameras = new Map(cameras.map((c) => [c.camId, c]));
    // Usually one hub; with several, the one heard from most recently.
    const latest = [...cameras].sort((a, b) => b.hubLastSeen - a.hubLastSeen)[0];
    return {
      lastContact: latest?.hubLastSeen ?? 0,
      nextContact: latest?.hubSchedule?.nextCheckinAt,
      hubBatPercent: latest?.hubBatPercent,
      cameras: cameras.map((c) => ({ camId: c.camId, name: c.name })),
    };
  }

  async recordings(camId: string): Promise<Recording[]> {
    const items = await this.request<MediaItem[]>('GET', `${this.camera(camId)}/media`);
    const name = this.cameras.get(camId)?.name ?? 'Kamera';
    return groupRecordings(items).map((files) => {
      const video = files.find((m) => m.fileType === 2);
      const photo = files.find((m) => m.fileType !== 2);
      // The video decides the state; a delete of any file shows as well.
      const main = video ?? photo ?? (files[0] as MediaItem);
      const deleting = files.find((m) => m.deleteState !== undefined);
      return {
        mediaId: main.mediaId,
        mediaIds: files.map((m) => m.mediaId),
        camId,
        fileType: video ? 2 : 0,
        cameraFile: cameraFileName(main.num),
        suffixes: [...new Set(files.map((m) => m.fileType).sort((a, b) => a - b))].map(
          cameraSuffix,
        ),
        mediaTime: main.mediaTime,
        thumbUrl: (photo ?? main).thumbUrl,
        videoUrl: video?.videoUrl,
        fileName: videoFileName(name, main.mediaTime, main.num),
        ...toState(deleting ?? main),
      };
    });
  }

  async requestVideos(camId: string, mediaIds: number[]): Promise<void> {
    for (const mediaId of mediaIds) {
      await this.request('POST', `${this.camera(camId)}/commands`, {
        type: 'requestVideo',
        mediaId,
      });
    }
  }

  async requestDelete(camId: string, mediaIds: number[]): Promise<void> {
    await this.request('POST', `${this.camera(camId)}/commands`, {
      type: 'deleteMedia',
      mediaIds,
    });
  }

  async withdraw(camId: string, recording: Recording): Promise<void> {
    try {
      for (const mediaId of recording.mediaIds) {
        await this.request('POST', `${this.camera(camId)}/media/${mediaId}/withdraw`);
      }
    } catch (error) {
      if (error instanceof ApiError && error.status === 409) {
        throw new Error('Der Hub hat den Auftrag schon erhalten, Zurücknehmen geht nicht mehr.');
      }
      throw error;
    }
  }

  async download(recording: Recording): Promise<Blob> {
    if (!recording.videoUrl) throw new Error('Das Video ist noch nicht da.');
    const response = await this.fetchFn(recording.videoUrl);
    if (!response.ok) throw new Error(`Download fehlgeschlagen (HTTP ${response.status})`);
    return response.blob();
  }

  private camera(camId: string): string {
    return `/v1/cameras/${encodeURIComponent(camId)}`;
  }

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    const headers: Record<string, string> = {
      Accept: 'application/json',
      Authorization: `Bearer ${this.token}`,
    };
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const response = await this.fetchFn(this.baseUrl + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) {
      throw new ApiError(response.status, `${method} ${path}: HTTP ${response.status}`);
    }
    return (await response.json()) as T;
  }
}
