import { ApiError } from '../api/http';
import type { Camera, MediaItem } from '../api/types';
import { cameraFile, videoFileName } from './text';
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
 * At most this far apart (camera seconds) are a video and its photo. Seen on
 * a TC100: 0-12 s, depending on the video's length.
 */
const PAIR_WINDOW_S = 120;

/**
 * The files of one recording: the camera takes a photo with each video and
 * stores both with neighbouring numbers in the same folder, e.g.
 * DSCF0023.AVI and DSCF0024.JPG. A video is paired with a photo whose number
 * is one off (the one after first, as the TC100 stores them) and that was
 * taken within PAIR_WINDOW_S; files without a partner stay alone. Keeps the order of the
 * backend's list (newest first).
 */
export function groupRecordings(items: MediaItem[]): MediaItem[][] {
  const photos = new Map<string, MediaItem>();
  for (const m of items) if (m.fileType !== 2) photos.set(`${m.dir}/${m.num}`, m);
  const paired = new Map<number, MediaItem>(); // photo mediaId -> its video
  const partner = new Map<number, MediaItem>(); // video mediaId -> its photo
  for (const video of items) {
    if (video.fileType !== 2) continue;
    for (const num of [video.num + 1, video.num - 1]) {
      const photo = photos.get(`${video.dir}/${num}`);
      if (
        photo &&
        !paired.has(photo.mediaId) &&
        Math.abs(photo.mediaTime - video.mediaTime) <= PAIR_WINDOW_S
      ) {
        paired.set(photo.mediaId, video);
        partner.set(video.mediaId, photo);
        break;
      }
    }
  }
  const groups: MediaItem[][] = [];
  for (const m of items) {
    if (m.fileType === 2) {
      const photo = partner.get(m.mediaId);
      groups.push(photo ? [photo, m] : [m]);
    } else if (!paired.has(m.mediaId)) {
      groups.push([m]);
    }
  }
  return groups;
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
        cameraFiles: files.map((m) => cameraFile(m.num, m.fileType)),
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
