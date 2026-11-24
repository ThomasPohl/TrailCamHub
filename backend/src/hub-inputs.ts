import type { CameraRecord, CommandRecord, FirmwareChange, HubRecord } from './model';
import type { Store } from './store/types';

/** What hubFaults() needs about one hub, read from the store (#265). */
export interface HubInputs {
  records: Array<{ camera: CameraRecord; commands: CommandRecord[] }>;
  firmwareChanges: FirmwareChange[];
  /** Cameras the user stopped reading (#297). */
  excluded: Set<string>;
}

/** How many firmware events are read from the store before they are sorted out. */
const FIRMWARE_EVENTS_READ = 30;
/** How many events of each kind are kept: held-back releases and all others. */
const FIRMWARE_EVENTS_KEPT = 5;

/**
 * The last five results other than `skipped` plus the last five `skipped`
 * events, newest first (#363). Held-back releases are common (a change of
 * reason adds one) and must not push a failure or rollback out of the window,
 * which would also drop the fault computed from it.
 */
export function selectFirmwareChanges(changes: FirmwareChange[]): FirmwareChange[] {
  const newestFirst = [...changes].sort((a, b) => b.at - a.at);
  const skipped = newestFirst.filter((c) => c.result === 'skipped').slice(0, FIRMWARE_EVENTS_KEPT);
  const others = newestFirst.filter((c) => c.result !== 'skipped').slice(0, FIRMWARE_EVENTS_KEPT);
  return [...skipped, ...others].sort((a, b) => b.at - a.at);
}

export async function loadHubInputs(store: Store, hub: HubRecord): Promise<HubInputs> {
  const records: HubInputs['records'] = [];
  for (const camId of await store.listHubCameraIds(hub.hubId)) {
    const camera = await store.getCamera(camId);
    if (camera && camera.hubId === hub.hubId) {
      records.push({ camera, commands: await store.listCommands(camId) });
    }
  }
  const firmwareChanges = selectFirmwareChanges(
    await store.listFirmwareChanges(hub.hubId, FIRMWARE_EVENTS_READ),
  );
  const excluded = new Set(
    (await store.listFoundCameras(hub.hubId))
      .filter((c) => c.state === 'excluded' && c.camId)
      .map((c) => c.camId as string),
  );
  return { records, firmwareChanges, excluded };
}
