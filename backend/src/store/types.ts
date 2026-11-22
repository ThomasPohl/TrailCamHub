import type {
  AppUser,
  AuditEntry,
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
  FirmwareChange,
  FoundCamera,
  Sample,
  SampleOwner,
  ScheduleConfig,
  VideoState,
} from '../model';

export type HubTelemetry = Partial<
  Pick<
    HubRecord,
    | 'firmware'
    | 'batPercent'
    | 'schedule'
    | 'uplink'
    | 'modemSignalDbm'
    | 'modemOperator'
    | 'memory'
  >
>;
export type CameraTelemetry = Partial<
  Pick<
    CameraRecord,
    | 'model'
    | 'firmware'
    | 'batPercent'
    | 'sdFreeMB'
    | 'sdTotalMB'
    | 'sdCard'
    | 'sdError'
    | 'temperature'
    | 'isCharging'
    | 'isSolarCharging'
    | 'batType'
    | 'bleVersion'
    | 'cameraName'
    | 'bleAddress'
    | 'settings'
    | 'capability'
  >
>;

/** Persistence boundary; implemented by DynamoDB in AWS and in memory for tests. */
export interface Store {
  putHub(hub: HubRecord): Promise<void>;
  getHub(hubId: string): Promise<HubRecord | undefined>;
  /** Every hub, sorted by id; for the admin (#259). */
  listHubIds(): Promise<string[]>;
  updateHub(hubId: string, telemetry: HubTelemetry, lastSeen: number): Promise<void>;
  /** Replaces the schedule set in the app (an empty object removes it). */
  setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void>;

  /**
   * Creates the camera for `hubId` unless it exists. Returns the stored record
   * and whether it was created now.
   */
  claimCamera(camId: string, hubId: string): Promise<{ camera: CameraRecord; created: boolean }>;
  getCamera(camId: string): Promise<CameraRecord | undefined>;
  /** Sets the name the user sees (#273); the camId stays the key. */
  renameCamera(camId: string, name: string): Promise<void>;
  /** Hides a camera in the gallery or shows it again (#273). */
  setCameraHidden(camId: string, hidden: boolean): Promise<void>;
  updateCamera(camId: string, telemetry: CameraTelemetry, lastSeen: number): Promise<void>;
  listHubCameraIds(hubId: string): Promise<string[]>;

  /** Links both ways: the user sees the hub, the hub knows its users. */
  linkUser(userId: number, hubId: string): Promise<void>;
  listUserHubIds(userId: number): Promise<string[]>;
  isLinked(userId: number, hubId: string): Promise<boolean>;
  listHubUserIds(hubId: string): Promise<number[]>;

  putAppUser(user: AppUser): Promise<void>;
  getAppUser(userId: number): Promise<AppUser | undefined>;

  /** Appends to the admin audit log (#259). */
  addAudit(entry: AuditEntry): Promise<void>;
  /** The newest `limit` entries, newest first. */
  listAudit(limit: number): Promise<AuditEntry[]>;

  listMedia(camId: string): Promise<MediaRecord[]>;
  getMedia(camId: string, mediaId: number): Promise<MediaRecord | undefined>;
  /** Inserts new media; never overwrites existing records (their flags survive). */
  addMedia(camId: string, items: MediaRecord[]): Promise<void>;
  /** Drops recordings that are no longer on the camera; unknown ids are ignored. */
  removeMedia(camId: string, mediaIds: number[]): Promise<void>;
  markThumbPresent(camId: string, mediaId: number): Promise<void>;
  setVideoState(camId: string, mediaId: number, state: VideoState): Promise<void>;

  /** Stores a battery/SD sample (#76); expires after HISTORY_KEEP_S. */
  addSample(owner: SampleOwner, sample: Sample): Promise<void>;
  /** Samples from `since` on, oldest first. */
  listSamples(owner: SampleOwner, since: number): Promise<Sample[]>;

  /** Cameras a hub found, by BLE address (#297). */
  getFoundCamera(hubId: string, bleAddress: string): Promise<FoundCamera | undefined>;
  putFoundCamera(hubId: string, camera: FoundCamera): Promise<void>;
  listFoundCameras(hubId: string): Promise<FoundCamera[]>;

  /** Records a firmware change of a hub (#149); kept for HISTORY_KEEP_S. */
  addFirmwareChange(hubId: string, change: FirmwareChange): Promise<void>;
  /** The newest `limit` changes, newest first. */
  listFirmwareChanges(hubId: string, limit: number): Promise<FirmwareChange[]>;

  /** The faults the admin has been mailed about and that still hold (#265). */
  getNotifiedFaults(): Promise<string[]>;
  putNotifiedFaults(keys: string[], at: number): Promise<void>;

  putCommand(camId: string, command: CommandRecord): Promise<void>;
  getCommand(camId: string, id: string): Promise<CommandRecord | undefined>;
  listCommands(camId: string): Promise<CommandRecord[]>;
}
