import type {
  AppUser,
  AuditEntry,
  ShareRecord,
  CameraRecord,
  CommandRecord,
  DeleteTarget,
  HubRecord,
  MediaRecord,
  FirmwareChange,
  FoundCamera,
  DataUsageDay,
  DataUsageReport,
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
    | 'powerMode'
    | 'schedule'
    | 'uplink'
    | 'modemSignalDbm'
    | 'modemOperator'
    | 'memory'
    | 'features'
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
  /**
   * Stores what the check-in reported; fields left `undefined` keep their value.
   * Fields named in `clear` are removed instead, so a value the hub no longer
   * reports does not stay behind as if it were current.
   */
  updateHub(
    hubId: string,
    telemetry: HubTelemetry,
    lastSeen: number,
    clear?: ReadonlyArray<keyof HubTelemetry>,
  ): Promise<void>;
  /** Replaces the schedule set in the app (an empty object removes it). */
  setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void>;
  /** Sets where a delete applies for the hub (#461); the same for every user. */
  setHubDeleteTargets(hubId: string, targets: DeleteTarget[]): Promise<void>;
  /** Sets the monthly data plan in MB (#415); undefined removes it. */
  setHubDataBudget(hubId: string, budgetMB: number | undefined): Promise<void>;
  /** Adds bytes to the sum of a hub's UTC day (`YYYY-MM-DD`) and transport (#415). */
  addDataUsage(hubId: string, day: string, report: DataUsageReport): Promise<void>;
  /** The day sums from `sinceDay` on (`YYYY-MM-DD`), oldest first. */
  listDataUsage(hubId: string, sinceDay: string): Promise<DataUsageDay[]>;

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
  /** Marks a recording as deleted in the gallery (#461); the record stays as a tombstone. */
  markMediaDeleted(camId: string, mediaId: number): Promise<void>;
  /** Marks a recording as no longer on the camera but kept (#461). */
  markMediaOffCamera(camId: string, mediaId: number): Promise<void>;
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

  /** Share links (#444): looked up by token hash, listed and revoked per owner. */
  putShare(share: ShareRecord): Promise<void>;
  getShareByTokenHash(tokenHash: string): Promise<ShareRecord | undefined>;
  getUserShare(userId: number, shareId: string): Promise<ShareRecord | undefined>;
  /** The user's links, newest first (expired ones may still be listed until DynamoDB removes them). */
  listUserShares(userId: number): Promise<ShareRecord[]>;
  deleteShare(share: ShareRecord): Promise<void>;

  putCommand(camId: string, command: CommandRecord): Promise<void>;
  getCommand(camId: string, id: string): Promise<CommandRecord | undefined>;
  listCommands(camId: string): Promise<CommandRecord[]>;
}
