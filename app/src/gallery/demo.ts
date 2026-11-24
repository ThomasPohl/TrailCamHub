import { placeholderThumb } from '../api/demo';
import { ApiError } from '../api/http';
import { toCameraWallClock } from '../format';
import type { Camera, CameraCapability, CameraSettings, ScheduleConfig } from '../api/types';
import type {
  AdminHub,
  AuditEntry,
  GalleryApi,
  FoundCamera,
  GalleryCamera,
  GalleryStatus,
  HubHistory,
  HubUsage,
  Me,
  Recording,
  ShareLink,
  ShareOptions,
} from './types';
import { cameraFile, videoFileName } from './text';

/** Settings as a PH970S reports them (the mock's capture, #368). */
const DEMO_SETTINGS: CameraSettings = {
  workMode: 0,
  shootMode: 2,
  picResolution: 6,
  videoResolution: 2,
  continuousShootCnt: 1,
  recordDuration: 10,
  recordAudio: 1,
  timelapseInterval: 3600,
  pirSens: 2,
  pirCoolingTime: 20,
  workTimeOnOff: 0,
  workTimeStartHour: 0,
  workTimeStartMin: 0,
  workTimeEndHour: 23,
  workTimeEndMin: 59,
  infraRedLum: 3,
  osdInfoEnable: true,
  WiFiLedEnable: true,
  dateFormat: 1,
  timeFormat: 0,
  languageCode: 'en',
  micVol: 90,
};

const DEMO_CAPABILITY: CameraCapability = {
  imgResolutions: [1, 2, 4, 6, 8, 9, 10],
  videoResolutions: [1, 2, 3, 4, 5],
  languages: ['en', 'de', 'fr', 'es', 'du', 'it', 'pl', 'cn', 'jp', 'ru'],
};

const CAMERAS: GalleryCamera[] = [
  {
    camId: 'F4C15606ED3B',
    name: 'Garten',
    settings: { ...DEMO_SETTINGS },
    capability: DEMO_CAPABILITY,
  },
  {
    camId: 'A1B2C3D4E5F6',
    name: 'Wiese',
    settings: { ...DEMO_SETTINGS, workMode: 1 },
    capability: DEMO_CAPABILITY,
  },
];

/** How often the simulated hub "checks in" in the demo. */
export const DEMO_CHECKIN_S = 20;

function buildRecordings(camera: GalleryCamera, now: number, count: number): Recording[] {
  const list: Recording[] = [];
  for (let i = 0; i < count; i++) {
    const num = 120 - 2 * i; // each video takes a photo with it: two numbers
    const mediaTime = toCameraWallClock(now - i * 2_700 - 900);
    const mediaId = 4_136_037_000 + num + (camera.camId === 'A1B2C3D4E5F6' ? 500 : 0);
    list.push({
      mediaId,
      // The camera keeps a photo and a video under the same name.
      mediaIds: [mediaId, mediaId + 100_000],
      camId: camera.camId,
      fileType: 2,
      cameraFiles: [cameraFile(num, 0), cameraFile(num - 1, 2)],
      mediaTime,
      thumbUrl: placeholderThumb(`${camera.name} ${num}`, (num * 37) % 360),
      state: 'new',
      fileName: videoFileName(camera.name, mediaTime, num - 1),
    });
  }
  return list;
}

/**
 * In-memory backend with a simulated hub that checks in every
 * DEMO_CHECKIN_S seconds: it then delivers requested videos and deletes what
 * was marked. Lets the gallery be tried before the backend part exists.
 */
export class DemoGalleryApi implements GalleryApi {
  readonly isDemo = true;
  private readonly media = new Map<string, Recording[]>();
  private readonly cameras = CAMERAS.map((c) => ({ ...c }));
  private lastCheckin: number;
  private scheduleConfig: ScheduleConfig = {};
  private dataBudgetMB: number | undefined = 3000;
  private readonly readRequested = new Set<string>();
  // The demo hub hears its two cameras and one more in range (#298).
  private readonly found: FoundCamera[] = [
    {
      hubId: 'garten-hub',
      bleAddress: 'AA:BB:CC:00:77:A1',
      state: 'new',
      name: 'TC100_77A1',
      rssi: -78,
      lastSeen: 0,
    },
  ];

  constructor(
    private readonly now: () => number = () => Math.floor(Date.now() / 1000),
    private readonly checkinS = DEMO_CHECKIN_S,
    /** Signed in as the admin (`?demo=admin`), with the admin view. */
    private readonly admin = false,
    /** `?demo=admin-code`: the admin view asks for a code first (the code is `123456`). */
    private needsCode = false,
  ) {
    const start = this.now();
    this.lastCheckin = start;
    for (const camera of CAMERAS) this.media.set(camera.camId, buildRecordings(camera, start, 16));
  }

  async status(): Promise<GalleryStatus> {
    this.catchUp();
    return {
      lastContact: this.lastCheckin,
      nextContact: this.lastCheckin + this.checkinS,
      hubBatPercent: 84,
      cameras: this.cameras.map((c, i) => ({
        ...c,
        // Garten is fine; Wiese needs a look (#304).
        batPercent: i === 0 ? 76 : 18,
        sdCard: 'ok' as const,
        sdFreeMB: i === 0 ? 28_400 : 950,
        sdTotalMB: 30_500,
        lastRead: this.lastCheckin - 300,
      })),
    };
  }

  async recordings(camId: string): Promise<Recording[]> {
    this.catchUp();
    return this.list(camId)
      .filter((r) => r.state !== 'deleted')
      .map((r) => ({ ...r }));
  }

  async requestVideos(camId: string, mediaIds: number[]): Promise<void> {
    this.catchUp();
    for (const r of this.pick(camId, mediaIds)) {
      if (r.state === 'new' || r.state === 'failed') {
        r.state = 'videoRequested';
        r.failReason = undefined;
      }
    }
  }

  async requestDelete(camId: string, mediaIds: number[]): Promise<void> {
    this.catchUp();
    for (const r of this.pick(camId, mediaIds)) {
      if (r.state !== 'deleted') r.state = 'deleteRequested';
    }
  }

  async withdraw(camId: string, recording: Recording): Promise<void> {
    this.catchUp();
    const [r] = this.pick(camId, [recording.mediaId]);
    if (r && (r.state === 'videoRequested' || r.state === 'deleteRequested')) {
      r.state = r.videoUrl ? 'videoReady' : 'new';
    }
  }

  async download(recording: Recording): Promise<Blob> {
    const text =
      `Demo-Datei statt eines Videos.\n` +
      `Aufnahme ${recording.cameraFiles.join(' + ')} (${recording.fileName}).\n` +
      `Mit echtem Hub liegt hier das MP4.\n`;
    return new Blob([text], { type: 'text/plain' });
  }

  private readonly shareLinks: ShareLink[] = [];

  async shareRecording(
    recording: Recording,
    options: ShareOptions,
  ): Promise<ShareLink & { url: string }> {
    const id = `demo${this.shareLinks.length + 1}`;
    const now = this.now();
    const link: ShareLink = {
      id,
      camId: recording.camId,
      mediaId: recording.mediaId,
      kind: options.kind,
      showCamera: options.showCamera,
      created: now,
      expiresAt: now + options.days * 24 * 60 * 60,
    };
    this.shareLinks.push(link);
    return { ...link, url: `https://demo.example/v1/s/${id}` };
  }

  async shares(): Promise<ShareLink[]> {
    const now = this.now();
    return this.shareLinks.filter((l) => l.expiresAt > now).map((l) => ({ ...l }));
  }

  async revokeShare(id: string): Promise<void> {
    const index = this.shareLinks.findIndex((l) => l.id === id);
    if (index >= 0) this.shareLinks.splice(index, 1);
  }

  async renameCamera(camId: string, name: string): Promise<string> {
    const camera = this.cameras.find((c) => c.camId === camId);
    if (!camera) throw new Error(`Unbekannte Kamera ${camId}`);
    camera.name = name.trim();
    return camera.name;
  }

  async setCameraHidden(camId: string, hidden: boolean): Promise<void> {
    const camera = this.cameras.find((c) => c.camId === camId);
    if (!camera) throw new Error(`Unbekannte Kamera ${camId}`);
    camera.hidden = hidden || undefined;
  }

  async changeSettings(camId: string, settings: CameraSettings): Promise<void> {
    const camera = this.cameras.find((c) => c.camId === camId);
    if (!camera) throw new ApiError(404, 'Camera not found');
    camera.settingsPending = { ...camera.settingsPending, ...settings };
    camera.settingsState = 'requested';
    camera.settingsError = undefined;
  }

  async withdrawSettings(camId: string): Promise<boolean> {
    const camera = this.cameras.find((c) => c.camId === camId);
    if (!camera?.settingsPending) return false;
    camera.settingsPending = undefined;
    camera.settingsState = undefined;
    return true;
  }

  async foundCameras(): Promise<FoundCamera[]> {
    this.catchUp();
    const read: FoundCamera[] = this.cameras.map((c, i) => ({
      hubId: 'garten-hub',
      bleAddress: `F4:C1:56:06:ED:${(0x3b + i).toString(16).toUpperCase()}`,
      state: 'approved',
      camId: c.camId,
      name: c.name,
      rssi: -55 - 5 * i,
      lastSeen: this.lastCheckin,
    }));
    // A decision about one of the own cameras replaces its entry.
    const decided = (r: FoundCamera) => this.found.find((f) => f.bleAddress === r.bleAddress) ?? r;
    const others = this.found.filter((f) => !read.some((r) => r.bleAddress === f.bleAddress));
    return [...read.map(decided), ...others].map((f) => ({
      ...f,
      lastSeen: f.lastSeen || this.lastCheckin,
    }));
  }

  async requestScan(): Promise<{ requested: number; nextContact?: number }> {
    this.catchUp();
    for (const c of this.cameras) this.readRequested.add(c.camId);
    return { requested: this.cameras.length, nextContact: this.lastCheckin + this.checkinS };
  }

  async decideCamera(
    _hubId: string,
    bleAddress: string,
    state: 'approved' | 'excluded',
    name?: string,
  ): Promise<void> {
    const found = this.found.find((f) => f.bleAddress === bleAddress);
    if (found) {
      found.state = state;
      if (name) found.name = name;
      return;
    }
    // One of the demo hub's own cameras: stop reading it.
    const index = Number.parseInt(bleAddress.slice(-2), 16) - 0x3b;
    const camera = this.cameras[index];
    if (!camera) throw new Error(`Unbekannte Kamera ${bleAddress}`);
    this.found.push({
      hubId: 'garten-hub',
      bleAddress,
      state,
      camId: camera.camId,
      name: name ?? camera.name,
      lastSeen: this.lastCheckin,
    });
  }

  async me(): Promise<Me> {
    return { name: this.admin ? 'Admin (Vorführung)' : 'Vorführung', admin: this.admin };
  }

  async latestFirmware(): Promise<string | undefined> {
    return 'v0.1.14';
  }

  elevated(): boolean {
    return this.admin && !this.needsCode;
  }

  async elevate(code: string): Promise<void> {
    if (code !== '123456') throw new ApiError(401, 'Wrong code');
    this.needsCode = false;
  }

  async rotateUser(userId: number): Promise<{ name: string; token: string }> {
    this.requireSession();
    return { name: `User ${userId}`, token: `${userId}.${'demo'.repeat(11)}` };
  }

  async revokeUser(): Promise<void> {
    this.requireSession();
  }

  async adminAudit(): Promise<AuditEntry[]> {
    this.requireSession();
    const at = this.lastCheckin;
    return [
      { at, actor: 1_000_000_000_000_002, action: 'elevate' },
      { at: at - 3_600, actor: 1_000_000_000_000_002, action: 'elevateFailed' },
    ];
  }

  private requireSession(): void {
    if (!this.elevated()) throw new ApiError(403, 'Admin session required');
  }

  async adminHubs(): Promise<AdminHub[]> {
    this.catchUp();
    this.requireSession();
    const cameras: Camera[] = this.cameras.map((c, i) => ({
      camId: c.camId,
      name: c.name,
      hubId: 'garten-hub',
      hubName: 'Garten',
      model: 'TC100',
      firmware: '2.2.5.9',
      batPercent: i === 0 ? 76 : 18,
      sdFreeMB: i === 0 ? 28_400 : 950,
      sdTotalMB: 30_500,
      sdCard: 'ok',
      temperature: 7,
      lastSeen: this.lastCheckin - 300,
      hubLastSeen: this.lastCheckin,
      hubScheduleConfig: this.scheduleConfig,
      readRequested: this.readRequested.has(c.camId),
    }));
    return [
      {
        hubId: 'garten-hub',
        name: 'Garten',
        firmware: 'v0.1.13',
        uplink: 'modem',
        modemSignalDbm: -91,
        modemOperator: 'Telekom.de',
        memory: { cycle: 412, heapMin: 41_200, psramMin: 3_250_000, stackSession: 1_400 },
        batPercent: 84,
        lastSeen: this.lastCheckin,
        schedule: {
          cameraPlan: this.scheduleConfig.cameraPlan ?? '05:00-09:00=30,*=120',
          cameraPlanOrigin: this.scheduleConfig.cameraPlan ? 'remote' : 'local',
          checkinIntervalS: this.scheduleConfig.checkinIntervalS ?? this.checkinS,
          checkinOrigin: this.scheduleConfig.checkinIntervalS ? 'remote' : 'local',
          nextCheckinAt: this.lastCheckin + this.checkinS,
          sleeps: true,
        },
        scheduleConfig: { ...this.scheduleConfig },
        cameras,
        users: [
          { userId: 1_000_000_000_000_001, name: 'Anwender', role: 'user' },
          { userId: 1_000_000_000_000_002, name: 'Admin', role: 'admin' },
        ],
        firmwareChanges: [
          {
            at: this.lastCheckin - 86_400,
            from: 'v0.1.13',
            to: 'v0.1.14',
            result: 'skipped',
            reason: 'the hub battery is too low',
          },
          {
            at: this.lastCheckin - 3 * 86_400,
            from: 'v0.1.12',
            to: 'v0.1.13',
            result: 'installed',
          },
          { at: this.lastCheckin - 9 * 86_400, from: 'v0.1.11', to: 'v0.1.12' },
        ],
        faults: [
          { kind: 'cameraBatteryLow', level: 'warning', camId: 'A1B2C3D4E5F6', value: 18 },
          { kind: 'sdNearlyFull', level: 'warning', camId: 'A1B2C3D4E5F6', value: 950 },
        ],
      },
    ];
  }

  async adminUsage(): Promise<HubUsage> {
    this.requireSession();
    // Ten days of a modem hub: a few MB a day and a video now and then.
    const days = Array.from({ length: 10 }, (_, i) => {
      const at = this.now() - (9 - i) * 86_400;
      return {
        day: new Date(at * 1000).toISOString().slice(0, 10),
        transport: 'modem' as const,
        txBytes: (i % 3 === 0 ? 14_000_000 : 400_000) + i * 20_000,
        rxBytes: 150_000 + i * 5_000,
      };
    });
    return { budgetMB: this.dataBudgetMB, days };
  }

  async setDataBudget(_hubId: string, budgetMB: number | undefined): Promise<void> {
    this.requireSession();
    this.dataBudgetMB = budgetMB;
  }

  async adminHistory(): Promise<HubHistory> {
    this.requireSession();
    // A falling camera battery and a hub battery that the sun recharges.
    const days = Array.from({ length: 30 }, (_, i) => this.lastCheckin - (29 - i) * 86_400);
    return {
      hub: days.map((at, i) => ({ at, batPercent: 70 + Math.round(15 * Math.sin(i / 3)) })),
      cameras: this.cameras.map((c, k) => ({
        camId: c.camId,
        name: c.name,
        samples: days.map((at, i) => ({
          at,
          batPercent: Math.max(5, Math.round((k === 0 ? 95 : 60) - i * (k === 0 ? 1 : 1.5))),
          sdFreeMB: 30_000 - i * 400,
        })),
      })),
    };
  }

  async setSchedule(_hubId: string, config: ScheduleConfig): Promise<void> {
    this.scheduleConfig = { ...config };
  }

  async readCamera(camId: string): Promise<void> {
    this.readRequested.add(camId);
  }

  /** Runs the check-ins that would have happened since the last call. */
  private catchUp(): void {
    const now = this.now();
    while (now >= this.lastCheckin + this.checkinS) {
      this.lastCheckin += this.checkinS;
      this.readRequested.clear();
      // The demo hub passes a settings change on and the camera takes it.
      for (const camera of this.cameras) {
        if (!camera.settingsPending) continue;
        camera.settings = { ...camera.settings, ...camera.settingsPending };
        camera.settingsPending = undefined;
        camera.settingsState = undefined;
      }
      for (const list of this.media.values()) {
        for (const r of list) {
          if (r.state === 'deleteRequested') r.state = 'deleted';
          else if (r.state === 'videoRequested') {
            r.state = 'videoReady';
            r.videoUrl = `demo:${r.mediaId}`;
          }
        }
      }
    }
  }

  private list(camId: string): Recording[] {
    const list = this.media.get(camId);
    if (!list) throw new Error(`Unbekannte Kamera ${camId}`);
    return list;
  }

  private pick(camId: string, mediaIds: number[]): Recording[] {
    const ids = new Set(mediaIds);
    return this.list(camId).filter((r) => r.mediaIds.some((id) => ids.has(id)));
  }
}
