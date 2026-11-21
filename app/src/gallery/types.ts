import type { Camera, HubSchedule, ScheduleConfig } from '../api/types';

/**
 * The gallery's view of the backend (#258): one end user sorts recordings by
 * thumbnail on his laptop, asks for some videos and deletes the rest.
 *
 * Every state here is what the hub has confirmed, except the two "requested"
 * states, which only mean the request is stored and waits for the hub.
 */

export type RecordingState =
  /** Thumbnail only; nothing asked for yet. */
  | 'new'
  /** Video asked for; the hub fetches it at its next contact. */
  | 'videoRequested'
  /** The video is ready to be saved on the laptop. */
  | 'videoReady'
  /** Delete asked for; can be withdrawn until the hub picks it up. */
  | 'deleteRequested'
  /** Deleted on the camera, confirmed by the hub. */
  | 'deleted'
  /** The last request failed; `failReason` says why. */
  | 'failed';

/**
 * One recording as the user sees it: a video and the photo the camera takes
 * with it, which are two files with their own numbers, e.g. DSCF0011.JPG and
 * DSCF0012.AVI. The mediaIds stay the keys.
 */
export interface Recording {
  /** Key of the recording: the video's mediaId, else the photo's. */
  mediaId: number;
  /** Every file of the recording (photo and video). */
  mediaIds: number[];
  camId: string;
  /** 2 if the recording has a video, else 0 (photo only). */
  fileType: 0 | 2;
  /** The files' names on the camera's SD card, photo first, e.g. ["DSCF0011.JPG", "DSCF0012.AVI"]. */
  cameraFiles: string[];
  /** Camera wall clock read as if it were UTC (docs/clock.md). */
  mediaTime: number;
  thumbUrl: string;
  state: RecordingState;
  /** With `videoReady`: where to download the video. */
  videoUrl?: string;
  /** File name to save the video under on the laptop, without folder. */
  fileName: string;
  /** With `failed`: a reason the user can understand (German). */
  failReason?: string;
}

export interface GalleryCamera {
  camId: string;
  name: string;
  /** Hidden by the user (#273); listed only to show it again. */
  hidden?: boolean;
  /** The camera's state at its last read (#304); absent if unknown. */
  batPercent?: number;
  sdCard?: 'ok' | 'missing' | 'unknown';
  sdFreeMB?: number;
  sdTotalMB?: number;
  /** Last read, Unix seconds; 0 = never. */
  lastRead?: number;
}

/** Longest camera name the backend accepts. */
export const MAX_CAMERA_NAME = 40;

export interface GalleryStatus {
  /** Last contact with the hub, Unix seconds; 0 = never. */
  lastContact: number;
  /** Expected next contact, Unix seconds; absent if unknown. */
  nextContact?: number;
  /** Hub battery in percent; absent if unknown. */
  hubBatPercent?: number;
  cameras: GalleryCamera[];
}

/** Who is signed in; only the admin gets the admin view (#264). */
export interface Me {
  name: string;
  admin: boolean;
}

/** One line of the admin audit log (#259); `at` in Unix seconds. */
export interface AuditEntry {
  at: number;
  actor: number;
  action: 'elevate' | 'elevateFailed' | 'elevateLocked' | 'rotateUser' | 'revokeUser';
  target?: number;
}

/** A camera a hub heard in its scans (#297, #298). */
export interface FoundCamera {
  hubId: string;
  bleAddress: string;
  /** new: not decided; approved: the hub reads it; excluded: not read. */
  state: 'new' | 'approved' | 'excluded';
  /** Set once the hub has read it. */
  camId?: string;
  /** The camera's name, else the advertised name. */
  name: string;
  rssi?: number;
  /** When the hub heard it last, Unix seconds. */
  lastSeen: number;
}

/** A battery/SD sample (#76). */
export interface Sample {
  at: number;
  batPercent?: number;
  sdFreeMB?: number;
  temperature?: number;
}

/** The last 30 days of a hub and its cameras, oldest first. */
export interface HubHistory {
  hub: Sample[];
  cameras: Array<{ camId: string; name: string; samples: Sample[] }>;
}

/** Something the admin should look at (#265); `faultText()` words it. */
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
    | 'cameraNotRead';
  level: 'error' | 'warning';
  camId?: string;
  /** Percent, free MB, seconds overdue or number of failed requests. */
  value?: number;
  detail?: string;
  since?: number;
}

/** A hub in the admin view (#264), as `GET /v1/admin/hubs` returns it. */
export interface AdminHub {
  hubId: string;
  name: string;
  firmware?: string;
  batPercent?: number;
  /** How the last check-in came in (#149). */
  uplink?: 'wifi' | 'modem' | 'ethernet';
  /** Modem signal in dBm, only while the hub syncs over the modem. */
  modemSignalDbm?: number;
  /** Mobile network of the last modem connect; only while the hub syncs over the modem. */
  modemOperator?: string;
  /** Lowest free memory of the last reported cycle, bytes (#149). */
  memory?: {
    cycle: number;
    heapMin: number;
    psramMin?: number;
    stackSession?: number;
    stackUplink?: number;
    stackController?: number;
  };
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  schedule?: HubSchedule;
  scheduleConfig: ScheduleConfig;
  cameras: Camera[];
  users: Array<{ userId: number; name?: string; role?: 'user' | 'admin' }>;
  /** Errors first (#265). */
  faults: Fault[];
  /**
   * The last firmware events, newest first (#149); absent on older backends.
   * `result` only where the hub reported the update itself; `skipped` is a
   * release the hub held back (`to`) for `reason`.
   */
  firmwareChanges?: Array<{
    at: number;
    from: string;
    to: string;
    result?: 'installed' | 'rolledBack' | 'failed' | 'skipped';
    reason?: string;
  }>;
}

export interface GalleryApi {
  readonly isDemo: boolean;
  status(): Promise<GalleryStatus>;
  /** All recordings of a camera that are not deleted yet, newest first. */
  recordings(camId: string): Promise<Recording[]>;
  /** `mediaIds`: the videos' mediaIds. */
  requestVideos(camId: string, mediaIds: number[]): Promise<void>;
  /** `mediaIds`: every file of the recordings to delete. */
  requestDelete(camId: string, mediaIds: number[]): Promise<void>;
  /** Withdraws the requests for a recording the hub has not picked up yet. */
  withdraw(camId: string, recording: Recording): Promise<void>;
  /** The video file of a recording in state `videoReady`. */
  download(recording: Recording): Promise<Blob>;
  /** Gives the camera the name the user sees (#273); returns the stored name. */
  renameCamera(camId: string, name: string): Promise<string>;
  /** Hides the camera in the gallery or shows it again (#273). */
  setCameraHidden(camId: string, hidden: boolean): Promise<void>;
  /** Cameras the user's hubs heard, read or not (#298). */
  foundCameras(): Promise<FoundCamera[]>;
  /**
   * Asks every hub of the user to read its cameras at the next contact; the
   * scan finds new ones (#308). `requested` = camera reads asked for; 0 if no
   * hub has a camera yet (it scans at every planned read anyway).
   */
  requestScan(): Promise<{ requested: number; nextContact?: number }>;
  /** Take a found camera in (with a name) or stop reading it (#298). */
  decideCamera(
    hubId: string,
    bleAddress: string,
    state: 'approved' | 'excluded',
    name?: string,
  ): Promise<void>;
  me(): Promise<Me>;
  /** Admin only: every hub with cameras and users. */
  adminHubs(): Promise<AdminHub[]>;
  /** Admin only: battery history of a hub and its cameras (#76). */
  adminHistory(hubId: string): Promise<HubHistory>;
  /** Whether an admin session is active; without one the admin view asks for a code (#259). */
  elevated(): boolean;
  /**
   * Admin only: exchanges the 6-digit code from the authenticator app for a
   * 15-minute admin session (#259). Throws an `ApiError` (401 wrong code, 429
   * locked, 403 no second factor).
   */
  elevate(code: string): Promise<void>;
  /** Admin only: new token for a user; the old link stops working (#259). */
  rotateUser(userId: number): Promise<{ name: string; token: string }>;
  /** Admin only: locks a user out until he gets a new token (#259). */
  revokeUser(userId: number): Promise<void>;
  /** Admin only: the newest audit entries (#259). */
  adminAudit(): Promise<AuditEntry[]>;
  /** Replaces the schedule set in the app; an empty object leaves it to the hub. */
  setSchedule(hubId: string, config: ScheduleConfig): Promise<void>;
  /** Asks the hub to read the camera at its next check-in. */
  readCamera(camId: string): Promise<void>;
  /** Tag of the newest firmware release, undefined if unknown (#149). */
  latestFirmware(): Promise<string | undefined>;
}
