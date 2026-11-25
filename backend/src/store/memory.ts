import type {
  AppUser,
  AuditEntry,
  ShareRecord,
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
  DataUsageDay,
  DataUsageReport,
  FirmwareChange,
  FoundCamera,
  Sample,
  SampleOwner,
  DeleteTarget,
  ScheduleConfig,
  VideoState,
} from '../model';
import type { CameraTelemetry, HubTelemetry, Store } from './types';

function defined<T extends object>(values: T): Partial<T> {
  return Object.fromEntries(
    Object.entries(values).filter(([, value]) => value !== undefined),
  ) as Partial<T>;
}

/** In-memory store for tests and local experiments. */
export class MemoryStore implements Store {
  private readonly hubs = new Map<string, HubRecord>();
  private readonly cameras = new Map<string, CameraRecord>();
  private readonly links = new Map<number, Set<string>>();
  private readonly media = new Map<string, Map<number, MediaRecord>>();
  private readonly commands = new Map<string, Map<string, CommandRecord>>();
  private readonly appUsers = new Map<number, AppUser>();
  private readonly audit: AuditEntry[] = [];
  private readonly samples = new Map<string, Sample[]>();
  private readonly firmwareChanges = new Map<string, FirmwareChange[]>();
  private readonly usage = new Map<string, DataUsageDay & { hubId: string }>();
  private readonly found = new Map<string, Map<string, FoundCamera>>();
  private notifiedFaults: string[] = [];
  private readonly shares = new Map<string, ShareRecord>();

  async putHub(hub: HubRecord): Promise<void> {
    this.hubs.set(hub.hubId, { ...hub });
  }

  async getHub(hubId: string): Promise<HubRecord | undefined> {
    const hub = this.hubs.get(hubId);
    return hub && { ...hub };
  }

  async updateHub(
    hubId: string,
    telemetry: HubTelemetry,
    lastSeen: number,
    clear: ReadonlyArray<keyof HubTelemetry> = [],
  ): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    const updated = { ...hub, ...defined(telemetry), lastSeen };
    for (const field of clear) delete (updated as Record<string, unknown>)[field];
    this.hubs.set(hubId, updated);
  }

  async setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    this.hubs.set(hubId, { ...hub, scheduleConfig: defined(config) });
  }

  async setHubDeleteTargets(hubId: string, targets: DeleteTarget[]): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    this.hubs.set(hubId, { ...hub, deleteTargets: [...targets] });
  }

  async setHubDataBudget(hubId: string, budgetMB: number | undefined): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    const updated = { ...hub, dataBudgetMB: budgetMB };
    if (budgetMB === undefined) delete updated.dataBudgetMB;
    this.hubs.set(hubId, updated);
  }

  async addDataUsage(hubId: string, day: string, report: DataUsageReport): Promise<void> {
    const key = `${hubId}#${day}#${report.transport}`;
    const old = this.usage.get(key);
    this.usage.set(key, {
      day,
      transport: report.transport,
      txBytes: (old?.txBytes ?? 0) + report.txBytes,
      rxBytes: (old?.rxBytes ?? 0) + report.rxBytes,
      hubId,
    });
  }

  async listDataUsage(hubId: string, sinceDay: string): Promise<DataUsageDay[]> {
    return [...this.usage.values()]
      .filter((u) => u.hubId === hubId && u.day >= sinceDay)
      .sort((a, b) => a.day.localeCompare(b.day) || a.transport.localeCompare(b.transport))
      .map(({ day, transport, txBytes, rxBytes }) => ({ day, transport, txBytes, rxBytes }));
  }

  async claimCamera(camId: string, hubId: string) {
    const existing = this.cameras.get(camId);
    if (existing) return { camera: { ...existing }, created: false };
    const camera: CameraRecord = { camId, hubId, name: camId, lastSeen: 0 };
    this.cameras.set(camId, camera);
    return { camera: { ...camera }, created: true };
  }

  async getCamera(camId: string): Promise<CameraRecord | undefined> {
    const camera = this.cameras.get(camId);
    return camera && { ...camera };
  }

  async renameCamera(camId: string, name: string): Promise<void> {
    const camera = this.cameras.get(camId);
    if (!camera) throw new Error(`Unknown camera ${camId}`);
    this.cameras.set(camId, { ...camera, name });
  }

  async setCameraHidden(camId: string, hidden: boolean): Promise<void> {
    const camera = this.cameras.get(camId);
    if (!camera) throw new Error(`Unknown camera ${camId}`);
    this.cameras.set(camId, { ...camera, hidden });
  }

  async updateCamera(camId: string, telemetry: CameraTelemetry, lastSeen: number): Promise<void> {
    const camera = this.cameras.get(camId);
    if (!camera) throw new Error(`Unknown camera ${camId}`);
    this.cameras.set(camId, { ...camera, ...defined(telemetry), lastSeen });
  }

  async listHubCameraIds(hubId: string): Promise<string[]> {
    return [...this.cameras.values()]
      .filter((c) => c.hubId === hubId)
      .map((c) => c.camId)
      .sort();
  }

  async linkUser(userId: number, hubId: string): Promise<void> {
    const set = this.links.get(userId) ?? new Set<string>();
    set.add(hubId);
    this.links.set(userId, set);
  }

  async listHubIds(): Promise<string[]> {
    return [...this.hubs.keys()].sort();
  }

  async listUserHubIds(userId: number): Promise<string[]> {
    return [...(this.links.get(userId) ?? [])].sort();
  }

  async isLinked(userId: number, hubId: string): Promise<boolean> {
    return this.links.get(userId)?.has(hubId) ?? false;
  }

  async listHubUserIds(hubId: string): Promise<number[]> {
    return [...this.links.entries()]
      .filter(([, hubs]) => hubs.has(hubId))
      .map(([userId]) => userId)
      .sort((a, b) => a - b);
  }

  async putAppUser(user: AppUser): Promise<void> {
    this.appUsers.set(user.userId, { ...user });
  }

  async getAppUser(userId: number): Promise<AppUser | undefined> {
    const user = this.appUsers.get(userId);
    return user && { ...user };
  }

  async putShare(share: ShareRecord): Promise<void> {
    this.shares.set(share.tokenHash, { ...share });
  }

  async getShareByTokenHash(tokenHash: string): Promise<ShareRecord | undefined> {
    const share = this.shares.get(tokenHash);
    return share && { ...share };
  }

  async getUserShare(userId: number, shareId: string): Promise<ShareRecord | undefined> {
    return (await this.listUserShares(userId)).find((s) => s.shareId === shareId);
  }

  async listUserShares(userId: number): Promise<ShareRecord[]> {
    return [...this.shares.values()]
      .filter((s) => s.createdBy === userId)
      .sort((a, b) => b.created - a.created)
      .map((s) => ({ ...s }));
  }

  async deleteShare(share: ShareRecord): Promise<void> {
    this.shares.delete(share.tokenHash);
  }

  async addAudit(entry: AuditEntry): Promise<void> {
    this.audit.push({ ...entry });
  }

  async listAudit(limit: number): Promise<AuditEntry[]> {
    return this.audit
      .map((entry, index) => ({ entry, index }))
      .sort((a, b) => b.entry.at - a.entry.at || b.index - a.index)
      .slice(0, limit)
      .map((x) => ({ ...x.entry }));
  }

  async listMedia(camId: string): Promise<MediaRecord[]> {
    return [...(this.media.get(camId)?.values() ?? [])].map((m) => ({ ...m }));
  }

  async getMedia(camId: string, mediaId: number): Promise<MediaRecord | undefined> {
    const item = this.media.get(camId)?.get(mediaId);
    return item && { ...item };
  }

  async addMedia(camId: string, items: MediaRecord[]): Promise<void> {
    const map = this.media.get(camId) ?? new Map<number, MediaRecord>();
    for (const item of items) {
      if (!map.has(item.mediaId)) map.set(item.mediaId, { ...item });
    }
    this.media.set(camId, map);
  }

  async markMediaDeleted(camId: string, mediaId: number): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.deleted = true;
  }

  async markMediaOffCamera(camId: string, mediaId: number): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.onCamera = false;
  }

  async markThumbPresent(camId: string, mediaId: number): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.thumbPresent = true;
  }

  async setVideoState(camId: string, mediaId: number, state: VideoState): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.videoState = state;
  }

  async addSample(owner: SampleOwner, sample: Sample): Promise<void> {
    const key = sampleKey(owner);
    const list = (this.samples.get(key) ?? []).filter((s) => s.at !== sample.at);
    list.push({ ...sample });
    list.sort((a, b) => a.at - b.at);
    this.samples.set(key, list);
  }

  async listSamples(owner: SampleOwner, since: number): Promise<Sample[]> {
    return (this.samples.get(sampleKey(owner)) ?? [])
      .filter((s) => s.at >= since)
      .map((s) => ({ ...s }));
  }

  async getFoundCamera(hubId: string, bleAddress: string): Promise<FoundCamera | undefined> {
    const camera = this.found.get(hubId)?.get(bleAddress);
    return camera && { ...camera };
  }

  async getNotifiedFaults(): Promise<string[]> {
    return [...this.notifiedFaults];
  }

  async putNotifiedFaults(keys: string[]): Promise<void> {
    this.notifiedFaults = [...keys];
  }

  async putFoundCamera(hubId: string, camera: FoundCamera): Promise<void> {
    const hub = this.found.get(hubId) ?? new Map<string, FoundCamera>();
    hub.set(camera.bleAddress, { ...camera });
    this.found.set(hubId, hub);
  }

  async listFoundCameras(hubId: string): Promise<FoundCamera[]> {
    return [...(this.found.get(hubId)?.values() ?? [])]
      .sort((a, b) => a.bleAddress.localeCompare(b.bleAddress))
      .map((c) => ({ ...c }));
  }

  async addFirmwareChange(hubId: string, change: FirmwareChange): Promise<void> {
    const list = this.firmwareChanges.get(hubId) ?? [];
    list.push({ ...change });
    this.firmwareChanges.set(hubId, list);
  }

  async listFirmwareChanges(hubId: string, limit: number): Promise<FirmwareChange[]> {
    return [...(this.firmwareChanges.get(hubId) ?? [])]
      .sort((a, b) => b.at - a.at)
      .slice(0, limit)
      .map((c) => ({ ...c }));
  }

  async putCommand(camId: string, command: CommandRecord): Promise<void> {
    const map = this.commands.get(camId) ?? new Map<string, CommandRecord>();
    map.set(command.id, { ...command });
    this.commands.set(camId, map);
  }

  async getCommand(camId: string, id: string): Promise<CommandRecord | undefined> {
    const command = this.commands.get(camId)?.get(id);
    return command && { ...command };
  }

  async listCommands(camId: string): Promise<CommandRecord[]> {
    return [...(this.commands.get(camId)?.values() ?? [])]
      .map((c) => ({ ...c }))
      .sort((a, b) => a.created - b.created);
  }
}

function sampleKey(owner: SampleOwner): string {
  return 'hubId' in owner ? `HUB#${owner.hubId}` : `CAM#${owner.camId}`;
}
