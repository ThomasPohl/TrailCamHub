/**
 * TrailCamHub API v1 as seen by the app (see backend/README.md).
 *
 * A user sees every camera of the hubs linked to them; one hub can serve
 * several cameras. Field names follow the camera protocol where possible
 * (`dir`/`num` = mediaDirNum/mediaNum, `mediaTime` in Unix seconds).
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
}

/** 0 = photo, 2 = video, as reported in the camera's media list. */
export type FileType = 0 | 2;

export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';

export interface MediaItem {
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

export type CommandType = 'requestVideo';

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
  requestVideo(camId: string, item: Pick<MediaItem, 'dir' | 'num'>): Promise<CommandResult>;
}
