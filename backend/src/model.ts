/**
 * Domain types and the API v1 wire format.
 *
 * A **hub** is a TrailCamHub module; it authenticates with its own key and
 * reports for one or more **cameras**. Cameras are created automatically the
 * first time a hub reports them and then belong to that hub. Users are linked
 * to hubs and see all cameras of their hubs.
 *
 * App-facing shapes (`Camera`, `MediaItem`, `CommandResult`) mirror
 * app/src/api/types.ts.
 */

export type FileType = 0 | 2;
export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';
/** requestVideo: fetch a video (mediaId); readCamera: read the camera now. */
export type CommandType = 'requestVideo' | 'readCamera';
export type CommandState = 'queued' | 'delivered' | 'done' | 'failed';

/** Where the hub got a schedule setting from. */
export type ScheduleOrigin = 'built-in' | 'local' | 'remote';

/** The hub's wake-up schedule as it reports it with every check-in. */
export interface HubSchedule {
  /** Camera read plan in effect, e.g. "05:00-09:00=30,*=120" or "off". */
  cameraPlan: string;
  cameraPlanOrigin: ScheduleOrigin;
  /** Check-in interval in effect, seconds; 0 = only after camera reads. */
  checkinIntervalS: number;
  checkinOrigin: ScheduleOrigin;
  /** Planned next camera read and check-in, Unix seconds; absent if none. */
  nextCameraReadAt?: number;
  nextCheckinAt?: number;
  /** The hub sleeps between cycles: commands wait for its next check-in. */
  sleeps: boolean;
}

/**
 * Schedule set in the app for a hub. Sent with every check-in; it overrides
 * the hub's own settings while set. Absent fields leave them to the hub.
 */
export interface ScheduleConfig {
  cameraPlan?: string;
  checkinIntervalS?: number;
}

export interface HubRecord {
  hubId: string;
  name: string;
  /** SHA-256 (hex) of the hub key secret; the secret itself is never stored. */
  keyHash: string;
  firmware?: string;
  batPercent?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  /** As reported at the last check-in. */
  schedule?: HubSchedule;
  scheduleConfig?: ScheduleConfig;
}

export interface CameraRecord {
  camId: string;
  hubId: string;
  name: string;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  /** When the hub last read the camera, Unix seconds. */
  lastSeen: number;
}

/**
 * A recording is identified by its `mediaId`, per camera. `dir`/`num` are only
 * data: the camera reuses them after its card was cleared. See
 * docs/media-identity.md.
 */
export interface MediaRecord {
  mediaId: number;
  dir: number;
  num: number;
  fileType: FileType;
  mediaTime: number;
  thumbPresent: boolean;
  videoState: VideoState;
}

export interface CommandRecord {
  id: string;
  type: CommandType;
  /** The recording of a requestVideo; 0 for readCamera. */
  mediaId: number;
  state: CommandState;
  created: number;
  updated: number;
  error?: string;
}

// ---- App API ---------------------------------------------------------------

export interface Camera {
  camId: string;
  name: string;
  hubId: string;
  hubName: string;
  model: string;
  firmware: string;
  batPercent: number;
  sdFreeMB: number;
  hubBatPercent?: number;
  lastSeen: number;
  /** Last check-in of the hub, Unix seconds; 0 = never. */
  hubLastSeen: number;
  /** The hub's schedule as reported at its last check-in. */
  hubSchedule?: HubSchedule;
  /** Schedule set in the app for the hub. */
  hubScheduleConfig: ScheduleConfig;
  /** A camera read was asked for and has not been done yet. */
  readRequested: boolean;
}

export interface MediaItem {
  mediaId: number;
  dir: number;
  num: number;
  fileType: FileType;
  mediaTime: number;
  thumbUrl: string;
  videoState: VideoState;
}

export interface CommandResult {
  id: string;
  type: CommandType;
  state: CommandState;
}

// ---- Hub (device) API ------------------------------------------------------

export interface MediaEntry {
  mediaId: number;
  dir: number;
  num: number;
  fileType: number;
  mediaTime: number;
}

export interface CameraReport {
  camId: string;
  /** When the hub read the camera (hub clock, Unix seconds); optional. */
  readAt?: number;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  /**
   * Media not reported before (delta). The backend only adds entries, so
   * sending an item again is harmless.
   */
  media?: MediaEntry[];
}

export interface CheckinRequest {
  hubFirmware?: string;
  hubBatPercent?: number;
  schedule?: HubSchedule;
  cameras?: CameraReport[];
}

export interface CameraCheckin {
  camId: string;
  /** False if the camera belongs to another hub; nothing was stored for it. */
  accepted: boolean;
  /**
   * The backend has no media for this camera yet (new camera): the hub should
   * report its full media list at the next check-in.
   */
  resync: boolean;
  /** Missing thumbnails: PUT the JPEG to `url`, then report it via /ack. */
  thumbUploads: Array<{ mediaId: number; url: string }>;
  /** mediaId only for requestVideo. */
  commands: Array<{ id: string; type: CommandType; mediaId?: number }>;
}

export interface CheckinResponse {
  serverTime: number;
  cameras: CameraCheckin[];
  /** Schedule set in the app; empty if none, so the hub uses its own. */
  config: ScheduleConfig;
}

export interface CameraAck {
  camId: string;
  uploadedThumbs?: Array<{ mediaId: number }>;
  commands?: Array<{ id: string; state: 'done' | 'failed'; error?: string }>;
}

export interface AckRequest {
  cameras?: CameraAck[];
}

/** A camera's mediaId: an unsigned 32-bit value, 0 is not valid. */
export function isMediaId(value: unknown): value is number {
  return Number.isInteger(value) && (value as number) > 0 && (value as number) <= 0xffffffff;
}

/** Ids of hubs and cameras: safe as S3 key and URL path segments. */
export const ID_PATTERN = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/;
