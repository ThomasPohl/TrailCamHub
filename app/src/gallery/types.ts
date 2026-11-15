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

export interface Recording {
  mediaId: number;
  camId: string;
  /** 0 = photo, 2 = video (camera media list). */
  fileType: 0 | 2;
  /** Camera wall clock read as if it were UTC (docs/clock.md). */
  mediaTime: number;
  thumbUrl: string;
  state: RecordingState;
  /** With `videoReady`: where to download the video. */
  videoUrl?: string;
  /** File name to save the video under, without folder. */
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
  requestVideos(camId: string, mediaIds: number[]): Promise<void>;
  requestDelete(camId: string, mediaIds: number[]): Promise<void>;
  /** Withdraws a request the hub has not picked up yet. */
  withdraw(camId: string, mediaId: number): Promise<void>;
  /** The video file of a recording in state `videoReady`. */
  download(recording: Recording): Promise<Blob>;
}
