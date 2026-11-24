import { ApiError } from '../api/http';
import type { Camera, CameraSettings, MediaItem } from '../api/types';
import { compareThumbnails, judge, MAX_MISMATCH, splitMismatchedPairs } from './stamp';
import { fetchLatestRelease } from './release';
import { cameraFile, videoFileName } from './text';
import type { ScheduleConfig } from '../api/types';
import type {
  AdminHub,
  AuditEntry,
  GalleryApi,
  FoundCamera,
  GalleryStatus,
  HubHistory,
  HubUsage,
  Me,
  Recording,
  ShareLink,
  ShareOptions,
  RecordingState,
} from './types';

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

/** Most mediaIds in one delete request (MAX_DELETE_PER_REQUEST in the backend). */
const DELETE_BATCH = 200;

/**
 * At most this far apart (camera seconds) are a video and its photo. Seen on
 * a TC100: 0-12 s, depending on the video's length.
 */
const PAIR_WINDOW_S = 120;

/**
 * The files of one recording: the camera takes a photo with each video and
 * stores both with neighbouring numbers in the same folder, e.g.
 * DSCF0023.AVI and DSCF0024.JPG. A video is paired with a photo whose number
 * is one off (the one after it for every video first, as the TC100 stores
 * them, then the one before) and that was taken within PAIR_WINDOW_S; files
 * without a partner stay alone. Keeps the order of the backend's list
 * (newest first).
 */
export function groupRecordings(
  items: MediaItem[],
  log?: (rows: Record<string, unknown>[]) => void,
  /** `false` (`?pairing=off`, #347): work out the pairs and log them, but show every file alone. */
  pair = true,
): MediaItem[][] {
  const trace: Record<string, unknown>[] = [];
  const photos = new Map<string, MediaItem>();
  for (const m of items) if (m.fileType !== 2) photos.set(`${m.dir}/${m.num}`, m);
  const paired = new Map<number, MediaItem>(); // photo mediaId -> its video
  const partner = new Map<number, MediaItem>(); // video mediaId -> its photo
  // Two passes: first every video with the photo right after it (how the
  // TC100 stores them), only then the one before. In one pass, a video whose
  // own photo is gone (deleted, or its thumbnail not uploaded yet) took the
  // photo of the neighbouring pair, and both pairs fell apart.
  for (const offset of [1, -1]) {
    for (const video of items) {
      if (video.fileType !== 2 || partner.has(video.mediaId)) continue;
      const num = video.num + offset;
      const photo = photos.get(`${video.dir}/${num}`);
      const apart = photo ? Math.abs(photo.mediaTime - video.mediaTime) : undefined;
      const taken = photo ? paired.has(photo.mediaId) : undefined;
      const match = photo !== undefined && !taken && (apart ?? Infinity) <= PAIR_WINDOW_S;
      if (log) {
        trace.push({
          video: video.mediaId,
          videoFile: `${video.dir}/${video.num}`,
          videoTime: video.mediaTime,
          tries: `${video.dir}/${num}`,
          photo: photo?.mediaId ?? '–',
          photoTime: photo?.mediaTime ?? '–',
          apartS: apart ?? '–',
          alreadyPaired: taken ?? '–',
          result: match ? 'paired' : !photo ? 'no photo' : taken ? 'taken' : 'too far apart',
        });
      }
      if (photo && match) {
        paired.set(photo.mediaId, video);
        partner.set(video.mediaId, photo);
      }
    }
  }
  log?.(trace);
  if (!pair) return items.map((m) => [m]);
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
 * With `?debug`: for every video and the photos next to it, how much the date
 * and time line of the two thumbnails differs (#347). `textPx` is the text
 * that was compared, `differentPx` the part that is in only one of them.
 */
async function logStampDifferences(items: MediaItem[], fetchFn: typeof fetch): Promise<void> {
  const photos = new Map<string, MediaItem>();
  for (const m of items) if (m.fileType !== 2) photos.set(`${m.dir}/${m.num}`, m);
  const rows: Record<string, unknown>[] = [];
  for (const video of items) {
    if (video.fileType !== 2) continue;
    for (const offset of [1, -1]) {
      const photo = photos.get(`${video.dir}/${video.num + offset}`);
      if (!photo) continue;
      const apartS = Math.abs(photo.mediaTime - video.mediaTime);
      if (apartS > PAIR_WINDOW_S) continue;
      const result = await compareThumbnails(video.thumbUrl, photo.thumbUrl, fetchFn);
      rows.push({
        video: `${video.dir}/${video.num}`,
        photo: `${photo.dir}/${photo.num}`,
        apartS,
        differentPx: result?.different ?? 'n/a',
        textPx: result?.text ?? 'n/a',
        ratio: result ? Number(result.ratio.toFixed(3)) : 'n/a',
        verdict: judge(result) === undefined ? 'n/a' : judge(result) ? 'same moment' : 'different',
      });
    }
  }
  console.info(`[gallery] date and time line (same moment up to ratio ${MAX_MISMATCH}):`);
  console.table(rows);
}

/**
 * The gallery's backend: the TrailCamHub API v1 with an app token (#259),
 * `Authorization: Bearer <userId>.<secret>`.
 */
/** The part of `sessionStorage` the API needs. */
export interface SessionStorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

const SESSION_KEY = 'trailcamhub.adminSession';

interface StoredSession {
  value: string;
  expiresAt: number;
  /** The token the session was issued for. */
  token: string;
}

export class HttpGalleryApi implements GalleryApi {
  readonly isDemo = false;
  private readonly baseUrl: string;
  private cameras = new Map<string, Camera>();
  /** The admin session (#259): used instead of the token while it lasts. */
  private session?: { value: string; expiresAt: number };

  constructor(
    baseUrl: string,
    private readonly token: string,
    private readonly fetchFn: typeof fetch = (...args) => fetch(...args),
    /** With `?debug` in the address: log how videos and photos are paired. */
    private readonly debug = false,
    private readonly now: () => number = () => Math.floor(Date.now() / 1000),
    /** Where the admin session survives a reload (one browser tab). */
    private readonly storage?: SessionStorageLike,
    /** With `?pairing=off`: no tile joins a video and a photo (#347). */
    private readonly pairing = true,
  ) {
    this.baseUrl = baseUrl.replace(/\/+$/, '');
    this.session = this.restoreSession();
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
      hub: latest && {
        hubId: latest.hubId,
        cameraPlan: latest.hubSchedule?.cameraPlan,
        config: latest.hubScheduleConfig,
      },
      cameras: cameras.map((c) => ({
        camId: c.camId,
        name: c.name,
        hidden: c.hidden,
        batPercent: c.batPercent,
        sdCard: c.sdCard,
        sdFreeMB: c.sdFreeMB,
        sdTotalMB: c.sdTotalMB,
        lastRead: c.lastSeen,
        settings: c.settings,
        capability: c.capability,
        settingsPending: c.settingsPending,
        settingsState: c.settingsState,
        settingsError: c.settingsError,
      })),
    };
  }

  async recordings(camId: string): Promise<Recording[]> {
    const items = await this.request<MediaItem[]>('GET', `${this.camera(camId)}/media`);
    const name = this.cameras.get(camId)?.name ?? 'Kamera';
    const log = this.debug
      ? (rows: Record<string, unknown>[]) => {
          console.info(`[gallery] ${items.length} files of ${camId}:`);
          console.table(
            items.map((m) => ({
              mediaId: m.mediaId,
              file: `${m.dir}/${m.num}`,
              fileType: m.fileType,
              mediaTime: m.mediaTime,
            })),
          );
          console.info('[gallery] pairing:');
          console.table(rows);
          void logStampDifferences(items, this.fetchFn);
        }
      : undefined;
    const grouped = groupRecordings(items, log, this.pairing);
    // The numbers say which photo belongs to a video; the date and time in
    // the thumbnails have the last word (#347).
    const groups = this.pairing
      ? await splitMismatchedPairs(grouped, (video, photo) => this.sameMoment(video, photo))
      : grouped;
    return groups.map((files) => {
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

  /** Verdicts of the date and time check, per video and photo; only answers are kept. */
  private readonly sameMoments = new Map<string, boolean>();

  private async sameMoment(video: MediaItem, photo: MediaItem): Promise<boolean | undefined> {
    const key = `${video.mediaId}:${photo.mediaId}`;
    const known = this.sameMoments.get(key);
    if (known !== undefined) return known;
    const same = judge(await compareThumbnails(video.thumbUrl, photo.thumbUrl, this.fetchFn));
    if (same !== undefined) this.sameMoments.set(key, same);
    return same;
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
    // The backend takes at most DELETE_BATCH ids per request.
    for (let i = 0; i < mediaIds.length; i += DELETE_BATCH) {
      await this.request('POST', `${this.camera(camId)}/commands`, {
        type: 'deleteMedia',
        mediaIds: mediaIds.slice(i, i + DELETE_BATCH),
      });
    }
  }

  async withdraw(camId: string, recording: Recording): Promise<void> {
    try {
      for (const mediaId of recording.mediaIds) {
        await this.request('POST', `${this.camera(camId)}/media/${mediaId}/withdraw`);
      }
    } catch (error) {
      if (error instanceof ApiError && error.status === 409) {
        throw new Error('Der Hub hat den Auftrag schon erhalten, Zurücknehmen geht nicht mehr.', {
          cause: error,
        });
      }
      throw error;
    }
  }

  async shareRecording(
    recording: Recording,
    options: ShareOptions,
  ): Promise<ShareLink & { url: string }> {
    // The thumbnail and the video of a recording hang on its key mediaId.
    try {
      return await this.request<ShareLink & { url: string }>(
        'POST',
        `${this.camera(recording.camId)}/media/${recording.mediaId}/share`,
        options,
      );
    } catch (error) {
      if (error instanceof ApiError && error.status === 429) {
        throw new Error('Es gibt schon 50 aktive Links. Bitte erst einen zurückziehen.', {
          cause: error,
        });
      }
      if (error instanceof ApiError && error.status === 409) {
        throw new Error('Dafür ist noch kein Bild oder Video in der Cloud.', { cause: error });
      }
      throw error;
    }
  }

  async shares(): Promise<ShareLink[]> {
    return this.request<ShareLink[]>('GET', '/v1/shares');
  }

  async revokeShare(id: string): Promise<void> {
    await this.request('POST', `/v1/shares/${encodeURIComponent(id)}/revoke`);
  }

  async download(recording: Recording): Promise<Blob> {
    if (!recording.videoUrl) throw new Error('Das Video ist noch nicht da.');
    const response = await this.fetchFn(recording.videoUrl);
    if (!response.ok) throw new Error(`Download fehlgeschlagen (HTTP ${response.status})`);
    return response.blob();
  }

  async renameCamera(camId: string, name: string): Promise<string> {
    const result = await this.request<{ name: string }>('POST', `${this.camera(camId)}/name`, {
      name,
    });
    const camera = this.cameras.get(camId);
    if (camera) this.cameras.set(camId, { ...camera, name: result.name });
    return result.name;
  }

  async setCameraHidden(camId: string, hidden: boolean): Promise<void> {
    await this.request('POST', `${this.camera(camId)}/hidden`, { hidden });
  }

  async changeSettings(camId: string, settings: CameraSettings): Promise<void> {
    await this.request('POST', `${this.camera(camId)}/commands`, {
      type: 'setSettings',
      settings,
    });
  }

  async withdrawSettings(camId: string): Promise<boolean> {
    const result = await this.request<{ withdrawn: number }>(
      'POST',
      `${this.camera(camId)}/settings/withdraw`,
    );
    return result.withdrawn > 0;
  }

  async foundCameras(): Promise<FoundCamera[]> {
    const me = await this.request<{ hubs?: Array<{ hubId: string }> }>('GET', '/v1/me');
    const found: FoundCamera[] = [];
    for (const { hubId } of me.hubs ?? []) {
      const list = await this.request<Array<Omit<FoundCamera, 'hubId'>>>(
        'GET',
        `/v1/hubs/${encodeURIComponent(hubId)}/found-cameras`,
      );
      for (const c of list) found.push({ ...c, hubId });
    }
    return found;
  }

  async requestScan(): Promise<{ requested: number; nextContact?: number }> {
    const me = await this.request<{ hubs?: Array<{ hubId: string }> }>('GET', '/v1/me');
    let requested = 0;
    let nextContact: number | undefined;
    for (const { hubId } of me.hubs ?? []) {
      const result = await this.request<{ requested: number; nextCheckinAt?: number }>(
        'POST',
        `/v1/hubs/${encodeURIComponent(hubId)}/scan`,
        {},
      );
      requested += result.requested;
      if (result.nextCheckinAt !== undefined) {
        nextContact = Math.min(nextContact ?? Infinity, result.nextCheckinAt);
      }
    }
    return { requested, nextContact };
  }

  async decideCamera(
    hubId: string,
    bleAddress: string,
    state: 'approved' | 'excluded',
    name?: string,
  ): Promise<void> {
    await this.request(
      'POST',
      `/v1/hubs/${encodeURIComponent(hubId)}/found-cameras/${encodeURIComponent(bleAddress)}`,
      name === undefined ? { state } : { state, name },
    );
  }

  async me(): Promise<Me> {
    const me = await this.request<{ name: string; role: string }>('GET', '/v1/me');
    return { name: me.name, admin: me.role === 'admin' };
  }

  latestFirmware(): Promise<string | undefined> {
    return fetchLatestRelease(this.fetchFn);
  }

  adminHubs(): Promise<AdminHub[]> {
    return this.request<AdminHub[]>('GET', '/v1/admin/hubs');
  }

  adminHistory(hubId: string): Promise<HubHistory> {
    return this.request<HubHistory>('GET', `/v1/admin/hubs/${encodeURIComponent(hubId)}/history`);
  }

  adminUsage(hubId: string): Promise<HubUsage> {
    return this.request<HubUsage>('GET', `/v1/admin/hubs/${encodeURIComponent(hubId)}/usage`);
  }

  async setDataBudget(hubId: string, budgetMB: number | undefined): Promise<void> {
    await this.request<unknown>('POST', `/v1/admin/hubs/${encodeURIComponent(hubId)}/data-budget`, {
      budgetMB: budgetMB ?? null,
    });
  }

  elevated(): boolean {
    return this.session !== undefined && this.session.expiresAt > this.now();
  }

  async elevate(code: string): Promise<void> {
    this.session = undefined;
    // With the plain token: the session replaces it only afterwards.
    const result = await this.request<{ session: string; expiresAt: number }>(
      'POST',
      '/v1/admin/elevate',
      { code },
    );
    this.session = { value: result.session, expiresAt: result.expiresAt };
    this.saveSession();
  }

  rotateUser(userId: number): Promise<{ name: string; token: string }> {
    return this.request('POST', `/v1/admin/users/${userId}/rotate`, {});
  }

  async revokeUser(userId: number): Promise<void> {
    await this.request('POST', `/v1/admin/users/${userId}/revoke`, {});
  }

  adminAudit(): Promise<AuditEntry[]> {
    return this.request<AuditEntry[]>('GET', '/v1/admin/audit');
  }

  async setSchedule(hubId: string, config: ScheduleConfig): Promise<void> {
    // POST, not PUT: the backend's CORS allows no PUT (#286).
    await this.request('POST', `/v1/hubs/${encodeURIComponent(hubId)}/schedule`, {
      cameraPlan: config.cameraPlan ?? null,
      checkinIntervalS: config.checkinIntervalS ?? null,
      powerSetting: config.powerSetting ?? null,
    });
  }

  async readCamera(camId: string): Promise<void> {
    await this.request('POST', `${this.camera(camId)}/commands`, { type: 'readCamera' });
  }

  /**
   * A reload (Cmd+R) must not end the admin session (#345): without it the
   * admin sees only the hubs linked to his own account, usually none. The
   * session is short-lived and kept per tab, and only for the token it was
   * issued for.
   */
  private restoreSession(): { value: string; expiresAt: number } | undefined {
    try {
      const raw = this.storage?.getItem(SESSION_KEY);
      if (!raw) return undefined;
      const saved = JSON.parse(raw) as Partial<StoredSession> | null;
      if (
        !saved ||
        saved.token !== this.token ||
        typeof saved.value !== 'string' ||
        typeof saved.expiresAt !== 'number' ||
        saved.expiresAt <= this.now()
      ) {
        this.storage?.removeItem(SESSION_KEY);
        return undefined;
      }
      return { value: saved.value, expiresAt: saved.expiresAt };
    } catch {
      return undefined;
    }
  }

  private saveSession(): void {
    try {
      if (!this.storage) return;
      if (this.session) {
        const stored: StoredSession = { ...this.session, token: this.token };
        this.storage.setItem(SESSION_KEY, JSON.stringify(stored));
      } else {
        this.storage.removeItem(SESSION_KEY);
      }
    } catch {
      // Storage can be unavailable (private mode): then the session lasts
      // until the next reload, as before.
    }
  }

  private camera(camId: string): string {
    return `/v1/cameras/${encodeURIComponent(camId)}`;
  }

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    // An active admin session replaces the token, except to get one.
    const useSession = this.elevated() && path !== '/v1/admin/elevate';
    const headers: Record<string, string> = {
      Accept: 'application/json',
      Authorization: useSession ? `Admin ${this.session?.value}` : `Bearer ${this.token}`,
    };
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const response = await this.fetchFn(this.baseUrl + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) {
      if (useSession && response.status === 401) {
        // The session ended (or its token or secret was rotated): carry on
        // with the plain token, so the admin view asks for a code again.
        this.session = undefined;
        this.saveSession();
        return this.request<T>(method, path, body);
      }
      throw new ApiError(response.status, `${method} ${path}: HTTP ${response.status}`);
    }
    return (await response.json()) as T;
  }
}
