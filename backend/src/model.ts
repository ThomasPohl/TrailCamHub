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
/**
 * requestVideo: fetch a video (mediaId); readCamera: read the camera now;
 * deleteMedia: delete a recording on the camera (mediaId).
 */
export type CommandType = 'requestVideo' | 'readCamera' | 'deleteMedia';
/** `cancelled`: withdrawn in the app before the hub picked it up. */
export type CommandState = 'queued' | 'delivered' | 'done' | 'failed' | 'cancelled';

/** Where the hub got a schedule setting from. */
export type ScheduleOrigin = 'built-in' | 'local' | 'remote';

/**
 * Why the hub stretches its schedule: its own battery is critical (no camera
 * reads, rare check-ins) or low, or a camera's battery is low.
 */
export type ScheduleStretch = 'hubBatteryCritical' | 'hubBatteryLow' | 'cameraBatteryLow';

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
  /** Why the schedule is stretched; absent if it is not. */
  stretch?: ScheduleStretch;
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
  /** Hidden in the gallery by a user (#273); the hub still reads it. */
  hidden?: boolean;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  sdTotalMB?: number;
  sdCard?: SdCardState;
  /** The camera's error message for a missing card; empty when the card is fine. */
  sdError?: string;
  /** Temperature the camera measured at the read, °C. */
  temperature?: number;
  /** The camera's battery was charging at the read. */
  isCharging?: boolean;
  /** ...from a solar panel. */
  isSolarCharging?: boolean;
  /** The camera's code for its battery type (meaning not documented). */
  batType?: number;
  /** Version of the camera's BLE module. */
  bleVersion?: string;
  /** The name set on the camera itself (often a default such as "0000"). */
  cameraName?: string;
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

/**
 * A user of the gallery web app (#259), created by the admin. Signs in with
 * an app token `<userId>.<secret>`; only the hash of the secret is stored.
 * The ids are far above Telegram's, so both kinds of users share links.
 */
export interface AppUser {
  userId: number;
  name: string;
  role: 'user' | 'admin';
  tokenHash: string;
  created: number;
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

/**
 * A battery and SD sample of a hub or a camera (#76), at most one per hour
 * and owner; kept for HISTORY_KEEP_S.
 */
export interface Sample {
  /** Unix seconds: the hub's check-in, or when the hub read the camera. */
  at: number;
  batPercent?: number;
  sdFreeMB?: number;
  temperature?: number;
}

/**
 * The hub reported another firmware than at its check-in before (#149):
 * an update, a rollback or a flash over USB.
 */
export interface FirmwareChange {
  /** Check-in that reported the new version, Unix seconds. */
  at: number;
  from: string;
  to: string;
}

/** Who a sample belongs to. */
export type SampleOwner = { hubId: string } | { camId: string };

/** How long samples are kept: half a year. */
export const HISTORY_KEEP_S = 183 * 24 * 60 * 60;

/** Battery history for the admin view, newest last. */
export interface HubHistory {
  hub: Sample[];
  cameras: Array<{ camId: string; name: string; samples: Sample[] }>;
}

/** Something the admin should look at (#265); the app words it. */
export interface Fault {
  kind:
    | 'hubNeverSeen'
    | 'hubSilent'
    | 'hubBatteryCritical'
    | 'hubBatteryLow'
    | 'cameraBatteryCritical'
    | 'cameraBatteryLow'
    | 'sdMissing'
    | 'sdFull'
    | 'sdNearlyFull'
    | 'requestsFailed'
    | 'firmwareDowngrade';
  level: 'error' | 'warning';
  /** The camera it is about; absent for the hub. */
  camId?: string;
  /**
   * Percent for batteries, free MB for SD cards, seconds overdue for a
   * silent hub, the number of failed requests.
   */
  value?: number;
  /** The camera's or the hub's reason, as it reported it. */
  detail?: string;
  /** Last contact of a silent hub, or the latest failure, Unix seconds. */
  since?: number;
}

/** A hub as the admin view shows it (#264). */
export interface AdminHub {
  hubId: string;
  name: string;
  firmware?: string;
  batPercent?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  /** As reported at the last check-in. */
  schedule?: HubSchedule;
  /** Set in the app; empty = the hub's own settings. */
  scheduleConfig: ScheduleConfig;
  cameras: Camera[];
  /** Users linked to the hub; Telegram users have no name here. */
  users: Array<{ userId: number; name?: string; role?: 'user' | 'admin' }>;
  /** Errors first (#265). */
  faults: Fault[];
  /** The last firmware changes, newest first (#149). */
  firmwareChanges: FirmwareChange[];
}

export interface Camera {
  camId: string;
  name: string;
  /** Hidden in the gallery (#273); the gallery offers to show it again. */
  hidden?: boolean;
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
  /** Temperature the camera measured at the read, °C. */
  temperature?: number;
  /** The camera's battery was charging at the read. */
  isCharging?: boolean;
  /** ...from a solar panel. */
  isSolarCharging?: boolean;
  /** The camera's code for its battery type (meaning not documented). */
  batType?: number;
  /** Version of the camera's BLE module. */
  bleVersion?: string;
  /** The name set on the camera itself (often a default such as "0000"). */
  cameraName?: string;
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
  /** With videoState `failed`: why, as the hub reported it. */
  videoError?: string;
  /** A delete was asked for (`requested`, `delivered` to the hub) or failed. */
  deleteState?: 'requested' | 'delivered' | 'failed';
  deleteError?: string;
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
  /** Temperature the camera measured at the read, °C. */
  temperature?: number;
  /** The camera's battery was charging at the read. */
  isCharging?: boolean;
  /** ...from a solar panel. */
  isSolarCharging?: boolean;
  /** The camera's code for its battery type (meaning not documented). */
  batType?: number;
  /** Version of the camera's BLE module. */
  bleVersion?: string;
  /** The name set on the camera itself (often a default such as "0000"). */
  cameraName?: string;
  /**
   * Media not reported before (delta). The backend only adds entries, so
   * sending an item again is harmless.
   */
  media?: MediaEntry[];
  /**
   * mediaIds no longer on the camera's card (deleted on the camera or by the
   * hub). The backend drops these recordings; repeating an id is harmless.
   */
  removedMedia?: number[];
}

export interface CheckinRequest {
  /** Version of the hub firmware: a release tag (v1.2.3) or a commit hash. */
  hubFirmware?: string;
  /** Board and build variant (`esp32s3`, `release`): which image fits. */
  hubBoard?: string;
  hubVariant?: string;
  hubBatPercent?: number;
  /**
   * The most thumbnail uploads the hub takes from this check-in. A hub with
   * little memory keeps the response small with it; the backend's own limit
   * still applies.
   */
  maxThumbUploads?: number;
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

/** Longest camera name; it is shown as a tab and used in file names (#273). */
export const MAX_CAMERA_NAME = 40;

/**
 * A camera name as given by the user or the admin: trimmed, 1 to
 * MAX_CAMERA_NAME characters, no control characters. Undefined if not valid.
 */
export function validCameraName(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const name = value.trim();
  const chars = [...name];
  if (chars.length === 0 || chars.length > MAX_CAMERA_NAME) return undefined;
  if (chars.some((c) => c < ' ' || c === '\x7f')) return undefined;
  return name;
}

/** Ids of hubs and cameras: safe as S3 key and URL path segments. */
export const ID_PATTERN = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/;
