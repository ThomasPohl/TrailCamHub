import type {
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
  ScheduleConfig,
  VideoState,
} from '../model';

export type HubTelemetry = Partial<Pick<HubRecord, 'firmware' | 'batPercent' | 'schedule'>>;
export type CameraTelemetry = Partial<
  Pick<CameraRecord, 'model' | 'firmware' | 'batPercent' | 'sdFreeMB'>
>;

/** Persistence boundary; implemented by DynamoDB in AWS and in memory for tests. */
export interface Store {
  putHub(hub: HubRecord): Promise<void>;
  getHub(hubId: string): Promise<HubRecord | undefined>;
  updateHub(hubId: string, telemetry: HubTelemetry, lastSeen: number): Promise<void>;
  /** Replaces the schedule set in the app (an empty object removes it). */
  setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void>;

  /**
   * Creates the camera for `hubId` unless it exists. Returns the stored record
   * and whether it was created now.
   */
  claimCamera(camId: string, hubId: string): Promise<{ camera: CameraRecord; created: boolean }>;
  getCamera(camId: string): Promise<CameraRecord | undefined>;
  updateCamera(camId: string, telemetry: CameraTelemetry, lastSeen: number): Promise<void>;
  listHubCameraIds(hubId: string): Promise<string[]>;

  linkUser(userId: number, hubId: string): Promise<void>;
  listUserHubIds(userId: number): Promise<string[]>;
  isLinked(userId: number, hubId: string): Promise<boolean>;

  listMedia(camId: string): Promise<MediaRecord[]>;
  getMedia(camId: string, mediaId: number): Promise<MediaRecord | undefined>;
  /** Inserts new media; never overwrites existing records (their flags survive). */
  addMedia(camId: string, items: MediaRecord[]): Promise<void>;
  markThumbPresent(camId: string, mediaId: number): Promise<void>;
  setVideoState(camId: string, mediaId: number, state: VideoState): Promise<void>;

  putCommand(camId: string, command: CommandRecord): Promise<void>;
  getCommand(camId: string, id: string): Promise<CommandRecord | undefined>;
  listCommands(camId: string): Promise<CommandRecord[]>;
}
