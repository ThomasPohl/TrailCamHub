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

/** A hub in the admin view (#264), as `GET /v1/admin/hubs` returns it. */
export interface AdminHub {
  hubId: string;
  name: string;
  firmware?: string;
  batPercent?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
  schedule?: HubSchedule;
  scheduleConfig: ScheduleConfig;
  cameras: Camera[];
  users: Array<{ userId: number; name?: string; role?: 'user' | 'admin' }>;
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
  me(): Promise<Me>;
  /** Admin only: every hub with cameras and users. */
  adminHubs(): Promise<AdminHub[]>;
  /** Replaces the schedule set in the app; an empty object leaves it to the hub. */
  setSchedule(hubId: string, config: ScheduleConfig): Promise<void>;
  /** Asks the hub to read the camera at its next check-in. */
  readCamera(camId: string): Promise<void>;
}
