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
 * deleteMedia: delete a recording on the camera (mediaId); setSettings:
 * change the camera's settings (`settings`, #368).
 */
export type CommandType = 'requestVideo' | 'readCamera' | 'deleteMedia' | 'setSettings';
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
  /**
   * The hub's `power` setting (docs/modes.md). The hub stores it as its own
   * setting when its board can use it and applies it from the next start.
   * Absent: the hub keeps what it has.
   */
  powerSetting?: PowerSetting;
}

/** What the hub's `power` setting can be set to from the app (#197). */
export const POWER_SETTINGS = ['auto', 'mains', 'battery'] as const;
export type PowerSetting = (typeof POWER_SETTINGS)[number];

export interface HubRecord {
  hubId: string;
  name: string;
  /** SHA-256 (hex) of the hub key secret; the secret itself is never stored. */
  keyHash: string;
  firmware?: string;
  batPercent?: number;
  /** Power mode the hub runs in, as reported at the last check-in (#416). */
  powerMode?: HubPowerMode;
  /** How the last check-in came in (#149). */
  uplink?: HubUplink;
  /** Modem signal when it connected, dBm; only meaningful with uplink "modem". */
  modemSignalDbm?: number;
  /** Name of the mobile network at the last modem connect; only meaningful with uplink "modem". */
  modemOperator?: string;
  /** Lowest free memory of the last reported cycle (#149, #244). */
  memory?: HubMemory;
  /** Monthly data plan in MB, set by the admin (#415); absent = none. */
  dataBudgetMB?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  /** As reported at the last check-in. */
  schedule?: HubSchedule;
  scheduleConfig?: ScheduleConfig;
}

export type HubUplink = 'wifi' | 'modem' | 'ethernet';

/**
 * Data volume of a hub on one transport on one UTC day (#415): the sum of what
 * the hub reported at its check-ins that day. HTTP bytes plus a fixed share
 * for TLS and TCP/IP, so an estimate.
 */
export interface DataUsageDay {
  /** UTC day, `YYYY-MM-DD`. */
  day: string;
  transport: HubUplink;
  txBytes: number;
  rxBytes: number;
}

/** One entry of the check-in's `dataUsage` (bytes since the last check-in). */
export interface DataUsageReport {
  transport: HubUplink;
  txBytes: number;
  rxBytes: number;
}

/** The data volume of a hub for the admin view (#415). */
export interface HubUsage {
  /** Monthly plan in MB, set by the admin; absent = none. */
  budgetMB?: number;
  /** The previous and the current month (plus a day each side), oldest first. */
  days: DataUsageDay[];
}

/** Largest monthly budget the admin can set, MB. */
export const MAX_DATA_BUDGET_MB = 1_000_000;
export type HubPowerMode = 'mains' | 'battery';

/** The hub's memory report, bytes (check-in `memory`, #236). */
export interface HubMemory {
  cycle: number;
  /** Lowest free internal heap. */
  heapMin: number;
  /** Lowest free PSRAM; absent on boards without. */
  psramMin?: number;
  /** Lowest free stack of the tasks. */
  stackSession?: number;
  stackUplink?: number;
  stackController?: number;
}

/**
 * The camera's SD card as the hub last saw it: `ok` when its media list was
 * read, `missing` when the camera answered it with an error (no card).
 */
export type SdCardState = 'ok' | 'missing';

/**
 * The camera's settings as the hub last read them (#368): field names of the
 * camera's device info (`workMode`, `pirSens`, ...; see settings.ts and
 * docs/camera-settings.md), numbers, booleans (`osdInfoEnable`,
 * `WiFiLedEnable`) and the language code. Only fields the camera supports.
 */
export type CameraSettings = Record<string, number | boolean | string>;

/** What the camera offers: resolution codes and language codes. */
export interface CameraCapability {
  imgResolutions: number[];
  videoResolutions: number[];
  languages: string[];
}

export interface CameraRecord {
  camId: string;
  hubId: string;
  name: string;
  /** Hidden in the gallery by a user (#273); the hub does not read it. */
  hidden?: boolean;
  /** BLE address the hub reported with its last read (#296). */
  bleAddress?: string;
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
  /** The camera's settings and capability at the last read (#368). */
  settings?: CameraSettings;
  capability?: CameraCapability;
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
  /** The recording of a requestVideo or deleteMedia; 0 otherwise. */
  mediaId: number;
  /** setSettings: the fields to change. */
  settings?: CameraSettings;
  state: CommandState;
  created: number;
  updated: number;
  error?: string;
}

/**
 * A user of the gallery web app (#259), created by the admin. Signs in with
 * an app token `<userId>.<secret>`; only the hash of the secret is stored.
 */
export interface AppUser {
  userId: number;
  name: string;
  role: 'user' | 'admin';
  tokenHash: string;
  created: number;
  /** Mail address for hub notices (#445), set by the admin. Absent: no mail. */
  email?: string;
  /** Which notices are mailed to `email` (#445); everything off when absent. */
  notify?: UserNotify;
  /** Admin only (#259): base32 TOTP secret, the second factor for the admin session. */
  totpSecret?: string;
  /** Last accepted TOTP time step; a code works only once. */
  totpLastStep?: number;
  /** Wrong codes in a row; reset by a right one. */
  totpFailures?: number;
  /** Unix seconds until which codes are refused after too many wrong ones. */
  totpLockedUntil?: number;
}

/** Notices mailed to an end user (#445). */
export interface UserNotify {
  /** The hub has not checked in (and when it is back). */
  hubSilent: boolean;
  /** A camera was not read for a day although the hub checks in. */
  cameraNotRead: boolean;
  /** A battery is critical (hub or camera). */
  battery: boolean;
}

/** One line of the admin audit log (#259), kept for AUDIT_KEEP_S. */
export interface AuditEntry {
  at: number;
  /** User id of the admin (or the user who tried to become one). */
  actor: number;
  action: 'elevate' | 'elevateFailed' | 'elevateLocked' | 'rotateUser' | 'revokeUser';
  /** User the action was about, if any. */
  target?: number;
}

export const AUDIT_KEEP_S = 90 * 24 * 60 * 60;

// ---- App API ---------------------------------------------------------------

/**
 * A battery and SD sample of a hub or a camera (#76), at most one per hour
 * and owner; kept for HISTORY_KEEP_S.
 */
export interface Sample {
  /** Unix seconds: the hub's check-in, or when the hub read the camera. */
  at: number;
  batPercent?: number;
  /** The hub was charging at this value; the gauge reads 100 % then (#421). */
  charging?: boolean;
  /** Cell voltage of the hub, mV (#421). */
  millivolts?: number;
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
  /** Empty for a `skipped` event of a hub that was stopped before it read the manifest (#363). */
  to: string;
  /**
   * The hub's own record of an update (#149); absent for a change the
   * backend noticed between two check-ins (e.g. a USB flash).
   */
  result?: FirmwareUpdateResult;
  /** Why it failed or was rolled back, as the hub said. */
  reason?: string;
}

/**
 * `skipped`: a newer release was offered but the hub held it back (battery,
 * signal, an earlier rollback, too many failures); `reason` says why (#142).
 */
export type FirmwareUpdateResult = 'installed' | 'rolledBack' | 'failed' | 'skipped';

/** The hub's last firmware update, sent once in a check-in (#149). */
export interface FirmwareUpdateReport {
  from: string;
  to: string;
  result: FirmwareUpdateResult;
  reason?: string;
  /** Unix seconds from the hub's clock; absent if it was not set. */
  at?: number;
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
    | 'firmwareDowngrade'
    | 'firmwareUpdateFailed'
    | 'hubMemoryLow'
    | 'powerModeMismatch'
    | 'cameraNotRead';
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
  powerMode?: HubPowerMode;
  uplink?: HubUplink;
  /** Only while the hub syncs over the modem. */
  modemSignalDbm?: number;
  modemOperator?: string;
  memory?: HubMemory;
  /** Monthly data plan in MB (#415); absent = none set. */
  dataBudgetMB?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  /** As reported at the last check-in. */
  schedule?: HubSchedule;
  /** Set in the app; empty = the hub's own settings. */
  scheduleConfig: ScheduleConfig;
  cameras: Camera[];
  /** Users linked to the hub. */
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
  /** The camera's settings at the last read (#368); absent if not known. */
  settings?: CameraSettings;
  capability?: CameraCapability;
  /**
   * Changes asked for in the gallery that the camera has not confirmed yet,
   * merged (the latest value per field). `requested`: waits for the hub;
   * `delivered`: the hub has it.
   */
  settingsPending?: CameraSettings;
  settingsState?: 'requested' | 'delivered';
  /** The last change failed (and no newer one waits): why. */
  settingsError?: string;
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
  /** The camera's BLE address as the hub connected to it (#296). */
  bleAddress?: string;
  /** Settings and capability at the read (#368); raw, see settings.ts. */
  settings?: unknown;
  capability?: unknown;
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
  /** "mains" or "battery" (#416); older firmware does not send it. */
  hubPowerMode?: HubPowerMode;
  /** The cell is being charged (#421); older firmware does not send it. */
  hubCharging?: boolean;
  /** Cell voltage in mV (#421). */
  hubBatMillivolts?: number;
  /** Transport of this check-in and the modem's signal in dBm (#149). */
  hubUplink?: HubUplink;
  modemSignalDbm?: number;
  /** Optional: older firmware does not send it. */
  modemOperator?: string;
  /** Raw; see HubMemory. */
  memory?: unknown;
  /** Raw list of DataUsageReport (#415); older firmware does not send it. */
  dataUsage?: unknown;
  /**
   * The most thumbnail uploads the hub takes from this check-in. A hub with
   * little memory keeps the response small with it; the backend's own limit
   * still applies.
   */
  maxThumbUploads?: number;
  schedule?: HubSchedule;
  /** The hub's last firmware update, once (#149). */
  firmwareUpdate?: FirmwareUpdateReport;
  cameras?: CameraReport[];
  /**
   * Cameras the hub heard advertising, read or not (#296). A hub that sends
   * this (or a `bleAddress`) gets `approvedCameras` in the response.
   */
  discovered?: DiscoveredReport[];
}

/** A camera heard in a BLE scan without reading it (#296). */
export interface DiscoveredReport {
  bleAddress: string;
  /** Advertised name, e.g. "TC100_ED3B". */
  name?: string;
  rssi?: number;
  /** When the hub heard it last (hub clock, Unix seconds). */
  seenAt?: number;
}

/** What the user decided about a camera a hub found (#297). */
export type FoundCameraState = 'new' | 'approved' | 'excluded';

/** A camera a hub found, keyed by hub and BLE address (#297). */
export interface FoundCamera {
  bleAddress: string;
  state: FoundCameraState;
  /** The camera's id once the hub has read it. */
  camId?: string;
  advName?: string;
  rssi?: number;
  firstSeen: number;
  lastSeen: number;
  /** Name given before the first read; becomes the camera's name then. */
  alias?: string;
}

/** "AA:BB:CC:DD:EE:FF" from 12 hex digits with or without ':' or '-'. */
export function normalizeBleAddress(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const hex = value.replace(/[:-]/g, '').toUpperCase();
  if (!/^[0-9A-F]{12}$/.test(hex)) return undefined;
  return (hex.match(/../g) ?? []).join(':');
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
   * mediaId also for deleteMedia, settings for setSettings.
   */
  commands: Array<{
    id: string;
    type: CommandType;
    mediaId?: number;
    uploadUrl?: string;
    settings?: CameraSettings;
  }>;
}

export interface CheckinResponse {
  serverTime: number;
  cameras: CameraCheckin[];
  /** Schedule set in the app; empty if none, so the hub uses its own. */
  config: ScheduleConfig;
  /**
   * BLE addresses of the cameras the hub may read (#297, used by #299). Only
   * for hubs that report BLE addresses; absent: read every camera.
   */
  approvedCameras?: string[];
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
