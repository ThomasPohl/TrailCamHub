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

/**
 * The camera's SD card as the hub last saw it: `ok` when its media list was
 * read, `missing` when the camera answered it with an error (no card).
 */
export type SdCardState = 'ok' | 'missing';

export interface CameraRecord {
  camId: string;
  hubId: string;
  name: string;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  sdTotalMB?: number;
  sdCard?: SdCardState;
  /** The camera's error message for a missing card; empty when the card is fine. */
  sdError?: string;
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

/** Bot settings of a Telegram user (in a private chat, chat id = user id). */
export interface UserSettings {
  /** Messages about new recordings. */
  notify: boolean;
  /**
   * Quiet hours in minutes after midnight, in `timeZone`: messages in this
   * window arrive without sound. Absent (or equal) = no quiet hours. The
   * window may wrap around midnight (from 22:00 to 07:00).
   */
  quietFrom?: number;
  quietTo?: number;
  /** IANA time zone for the quiet hours, e.g. "Europe/Berlin". */
  timeZone: string;
}

/** A one-time code that links the Telegram user who redeems it to a hub. */
export interface PairingCode {
  code: string;
  hubId: string;
  /** Unix seconds; the code is refused afterwards (and DynamoDB deletes it). */
  expiresAt: number;
  /** Telegram user who created it with /invite; absent for admin or hub codes. */
  createdBy?: number;
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
  /** Size of the camera's SD card in MB; absent if unknown. */
  sdTotalMB?: number;
  /** `unknown` until the hub has read the camera's media list. */
  sdCard: SdCardState | 'unknown';
  /** The camera's error message when the card is missing. */
  sdError?: string;
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
  /** With videoState `available`: short-lived URL of the MP4. */
  videoUrl?: string;
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
  sdTotalMB?: number;
  /** `ok` or `missing` (see SdCardState); absent if the hub does not know. */
  sdCard?: string;
  /** With `missing`: the camera's error message, e.g. "Memory card not exist". */
  sdError?: string;
  /**
   * Media not reported before (delta). The backend only adds entries, so
   * sending an item again is harmless.
   */
  media?: MediaEntry[];
}

export interface CheckinRequest {
  /** Version of the hub firmware: a release tag (v1.2.3) or a commit hash. */
  hubFirmware?: string;
  /** Board and build variant (`esp32s3`, `release`): which image fits. */
  hubBoard?: string;
  hubVariant?: string;
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
  /**
   * mediaId and uploadUrl only for requestVideo. The hub PUTs the MP4
   * (Content-Type video/mp4) to `uploadUrl` and then acknowledges the command;
   * an open command comes again with every check-in, with a fresh URL.
   */
  commands: Array<{ id: string; type: CommandType; mediaId?: number; uploadUrl?: string }>;
}

export interface CheckinResponse {
  serverTime: number;
  cameras: CameraCheckin[];
  /** Schedule set in the app; empty if none, so the hub uses its own. */
  config: ScheduleConfig;
  /**
   * A newer firmware release for the hub's board and variant (docs/ota.md);
   * absent if there is none or the hub runs a development build.
   */
  firmware?: { version: string; url: string; sha256: string; size: number };
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
