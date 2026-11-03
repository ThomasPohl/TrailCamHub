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
  /** The TrailCamHub module that reads this camera. */
  hubId: string;
  hubName: string;
  model: string;
  firmware: string;
  /** Camera battery in percent (cmdId 512). */
  batPercent: number;
  /** Free space on the camera's SD card in MB (cmdId 512). */
  sdFreeMB: number;
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
  /** A camera read was asked for and has not been done yet. */
  readRequested: boolean;
}

/** Where the hub got a schedule setting from. */
export type ScheduleOrigin = 'built-in' | 'local' | 'remote';

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
}

export interface ScheduleConfig {
  cameraPlan?: string;
  checkinIntervalS?: number;
}

/** 0 = photo, 2 = video, as reported in the camera's media list. */
export type FileType = 0 | 2;

export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';

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
}

export type CommandType = 'requestVideo' | 'readCamera';

export interface CommandResult {
  id: string;
  type: CommandType;
  state: 'queued' | 'delivered' | 'done' | 'failed';
}

export interface ApiClient {
  /** True if the client returns demo data instead of talking to a backend. */
  readonly isDemo: boolean;
  listCameras(): Promise<Camera[]>;
  listMedia(camId: string): Promise<MediaItem[]>;
  requestVideo(camId: string, item: Pick<MediaItem, 'mediaId'>): Promise<CommandResult>;
  /** Asks the hub to read the camera at its next check-in. */
  readCamera(camId: string): Promise<CommandResult>;
  /** Replaces the schedule set in the app for a hub; returns what is stored. */
  setSchedule(hubId: string, config: ScheduleConfig): Promise<ScheduleConfig>;
}
