/**
 * TrailCamHub API v1 as seen by the app (see backend/README.md).
 *
 * A user sees every camera of the hubs linked to them; one hub can serve
 * several cameras. Field names follow the camera protocol where possible
 * (`dir`/`num` = mediaDirNum/mediaNum, `mediaTime` in Unix seconds).
 * A recording is identified by its `mediaId`; `dir`/`num` are only shown,
 * because the camera reuses them after its card was cleared.
 */

export interface Camera {
  camId: string;
  name: string;
  /** Hidden in the gallery (#273). */
  hidden?: boolean;
  /** The TrailCamHub module that reads this camera. */
  hubId: string;
  hubName: string;
  model: string;
  firmware: string;
  /** Camera battery in percent (cmdId 512). */
  batPercent: number;
  /** Free space on the camera's SD card in MB (cmdId 512). */
  sdFreeMB: number;
  /** Size of the camera's SD card in MB (cmdId 512); absent if unknown. */
  sdTotalMB?: number;
  /**
   * The camera's SD card at the hub's last read: `missing` when the camera
   * answered its media list with an error, as it does without a card.
   */
  sdCard: 'ok' | 'missing' | 'unknown';
  /** The camera's message for a missing card, e.g. "Memory card not exist". */
  sdError?: string;
  /** Temperature the camera measured at the hub's last read, °C. */
  temperature?: number;
  /** The camera's battery was charging at the hub's last read. */
  isCharging?: boolean;
  /** ...from a solar panel. */
  isSolarCharging?: boolean;
  /** The camera's code for its battery type (meaning not documented). */
  batType?: number;
  /** Version of the camera's BLE module. */
  bleVersion?: string;
  /** The name set on the camera itself (often a default such as "0000"). */
  cameraName?: string;
  /** Battery of the TrailCamHub module itself, in percent; absent if unknown. */
  hubBatPercent?: number;
  /** When the hub last read the camera, Unix seconds. */
  lastSeen: number;
  /** Last check-in of the hub, Unix seconds; 0 = never. */
  hubLastSeen: number;
  /** The hub's wake-up schedule as reported at its last check-in. */
  hubSchedule?: HubSchedule;
  /** Schedule set in the app for the hub; empty fields: the hub's own. */
  hubScheduleConfig: ScheduleConfig;
  /** Where a delete applies for this hub (#461); the same for every user. */
  hubDeleteTargets: DeleteTarget[];
  /**
   * The hub's firmware can delete on the hub, or on camera and hub (#461).
   * Without it only "camera" and "backend" (or both) are possible.
   */
  hubDeletesByTarget: boolean;
  /** A camera read was asked for and has not been done yet. */
  readRequested: boolean;
  /** The camera's settings at the hub's last read (#368); absent if unknown. */
  settings?: CameraSettings;
  capability?: CameraCapability;
  /** Changes asked for that the camera has not confirmed yet, merged. */
  settingsPending?: CameraSettings;
  /** `requested`: waits for the hub; `delivered`: the hub has it. */
  settingsState?: 'requested' | 'delivered';
  /** The last change failed: the hub's reason. */
  settingsError?: string;
}

/**
 * Camera settings by the field names of the camera's device info
 * (`workMode`, `pirSens`, ...; docs/camera-settings.md). Only the fields the
 * camera supports.
 */
export type CameraSettings = Record<string, number | boolean | string>;

/** What the camera offers: resolution codes and language codes. */
export interface CameraCapability {
  imgResolutions: number[];
  videoResolutions: number[];
  languages: string[];
}

/** Where the hub got a schedule setting from. */
export type ScheduleOrigin = 'built-in' | 'local' | 'remote' | 'gnss';

/**
 * Why the hub stretches its schedule: its own battery is critical (no camera
 * reads, rare check-ins) or low, or a camera's battery is low.
 */
export type ScheduleStretch = 'hubBatteryCritical' | 'hubBatteryLow' | 'cameraBatteryLow';

export interface HubSchedule {
  /** Camera read plan in effect, e.g. "05:00-09:00=30,*=120" or "off". */
  cameraPlan: string;
  cameraPlanOrigin: ScheduleOrigin;
  /** Check-in interval in effect, seconds; 0 = only after camera reads. */
  checkinIntervalS: number;
  checkinOrigin: ScheduleOrigin;
  /** Planned next camera read and check-in, Unix seconds. */
  nextCameraReadAt?: number;
  nextCheckinAt?: number;
  /** The hub sleeps between cycles: requests wait for its next check-in. */
  sleeps: boolean;
  /** Why the schedule is stretched; absent if it is not. */
  stretch?: ScheduleStretch;
  /** Quiet hours ("23:00-06:00" or "off") and their origin (#436, #474). */
  quiet?: string;
  quietOrigin?: ScheduleOrigin;
  /** Offset of the hub's local time to UTC at the check-in, minutes (CET: 60). */
  utcOffsetMin?: number;
  /** Location for the sun times ("52.5,13.4") and its origin. */
  location?: string;
  locationOrigin?: ScheduleOrigin;
  /** No location set: the sun times use a fallback in the middle of Germany. */
  locationMissing?: boolean;
  /** Today's sunrise and sunset, local minutes of the day. */
  sunriseMin?: number;
  sunsetMin?: number;
  /** Energy factor for the check-in interval, 1/1000 (500: twice as often), and the morning charge. */
  energyPermille?: number;
  energyMorning?: number;
}

/** What the hub's `power` setting can be set to from the app (#197). */
export type PowerSetting = 'auto' | 'mains' | 'battery';

export interface ScheduleConfig {
  cameraPlan?: string;
  checkinIntervalS?: number;
  /** Quiet hours ("23:00-06:00", "off"); absent: the hub's own (#436). */
  quiet?: string;
  /** Location for the sun times ("52.5,13.4"); absent: the hub's own (#436). */
  location?: string;
  /** The hub stores it if its board can use it; absent: it keeps what it has. */
  powerSetting?: PowerSetting;
}

/** 0 = photo, 2 = video, as reported in the camera's media list. */
export type FileType = 0 | 2;

export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';

/**
 * Where a delete applies (#461): `camera` the files on the camera's card,
 * `hub` the hub's copies, `backend` the gallery.
 */
export type DeleteTarget = 'camera' | 'hub' | 'backend';

export interface MediaItem {
  /** Identifies the recording within its camera (docs/media-identity.md). */
  mediaId: number;
  dir: number;
  num: number;
  fileType: FileType;
  /**
   * Capture time: the camera's wall clock read as if it were UTC, in seconds
   * (not a real instant; show it with formatCameraTime()). Unreliable until
   * the camera clock is set.
   */
  mediaTime: number;
  /** Short-lived URL of the thumbnail JPEG (S3 presigned URL in production). */
  thumbUrl: string;
  videoState: VideoState;
  /** With videoState `available`: short-lived URL of the MP4. */
  videoUrl?: string;
  /** With videoState `failed`: why, as the hub reported it. */
  videoError?: string;
  /** A delete was asked for, handed to the hub, or failed (#260). */
  deleteState?: 'requested' | 'delivered' | 'failed';
  deleteError?: string;
  /** Deleted on the camera, kept here (#461). */
  onCamera?: false;
}
