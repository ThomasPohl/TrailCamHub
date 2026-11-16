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

export interface GalleryStatus {
  /** Last contact with the hub, Unix seconds; 0 = never. */
  lastContact: number;
  /** Expected next contact, Unix seconds; absent if unknown. */
  nextContact?: number;
  /** Hub battery in percent; absent if unknown. */
  hubBatPercent?: number;
  cameras: GalleryCamera[];
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
}
