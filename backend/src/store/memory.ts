import type {
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
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

  async putHub(hub: HubRecord): Promise<void> {
    this.hubs.set(hub.hubId, { ...hub });
  }

  async getHub(hubId: string): Promise<HubRecord | undefined> {
    const hub = this.hubs.get(hubId);
    return hub && { ...hub };
  }

  async updateHub(hubId: string, telemetry: HubTelemetry, lastSeen: number): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    this.hubs.set(hubId, { ...hub, ...defined(telemetry), lastSeen });
  }

  async setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void> {
    const hub = this.hubs.get(hubId);
    if (!hub) throw new Error(`Unknown hub ${hubId}`);
    this.hubs.set(hubId, { ...hub, scheduleConfig: defined(config) });
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

  async listUserHubIds(userId: number): Promise<string[]> {
    return [...(this.links.get(userId) ?? [])].sort();
  }

  async isLinked(userId: number, hubId: string): Promise<boolean> {
    return this.links.get(userId)?.has(hubId) ?? false;
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

  async markThumbPresent(camId: string, mediaId: number): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.thumbPresent = true;
  }

  async setVideoState(camId: string, mediaId: number, state: VideoState): Promise<void> {
    const item = this.media.get(camId)?.get(mediaId);
    if (item) item.videoState = state;
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
