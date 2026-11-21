import type { CameraRecord, CommandRecord, FirmwareChange, HubRecord } from './model';
import type { Store } from './store/types';

/** What hubFaults() needs about one hub, read from the store (#265). */
export interface HubInputs {
  records: Array<{ camera: CameraRecord; commands: CommandRecord[] }>;
  firmwareChanges: FirmwareChange[];
  /** Cameras the user stopped reading (#297). */
  excluded: Set<string>;
}

export async function loadHubInputs(store: Store, hub: HubRecord): Promise<HubInputs> {
  const records: HubInputs['records'] = [];
  for (const camId of await store.listHubCameraIds(hub.hubId)) {
    const camera = await store.getCamera(camId);
    if (camera && camera.hubId === hub.hubId) {
      records.push({ camera, commands: await store.listCommands(camId) });
    }
  }
  const firmwareChanges = await store.listFirmwareChanges(hub.hubId, 5);
  const excluded = new Set(
    (await store.listFoundCameras(hub.hubId))
      .filter((c) => c.state === 'excluded' && c.camId)
      .map((c) => c.camId as string),
  );
  return { records, firmwareChanges, excluded };
}
