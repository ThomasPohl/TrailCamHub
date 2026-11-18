import { HttpError } from './http';
import { normalizeBleAddress, type FoundCamera } from './model';
import type { Store } from './store/types';

/**
 * Cameras a hub found (#296, #297): which ones it may read, and the names
 * given to them in the gallery. Keyed by hub and BLE address; a camera's id
 * (devId) is known only after the hub has read it once.
 */

/** Most discovered cameras taken from one check-in. */
export const MAX_DISCOVERED_PER_CHECKIN = 32;
/** Longest advertised name kept. */
const MAX_ADV_NAME = 32;

export interface DiscoveredEntry {
  bleAddress: string;
  name?: string;
  rssi?: number;
  seenAt: number;
}

/**
 * The `discovered` list of a check-in, checked: valid addresses only, each
 * once, plausible values. `undefined` if the hub did not send the field.
 */
export function parseDiscovered(
  raw: unknown,
  now: number,
  readTime: (at: unknown, now: number) => number,
): DiscoveredEntry[] | undefined {
  if (raw === undefined) return undefined;
  if (!Array.isArray(raw)) throw new HttpError(400, 'discovered must be an array');
  if (raw.length > MAX_DISCOVERED_PER_CHECKIN) {
    throw new HttpError(413, 'Too many discovered cameras');
  }
  const byAddress = new Map<string, DiscoveredEntry>();
  for (const [i, item] of raw.entries()) {
    const entry = item as Record<string, unknown> | null;
    const bleAddress = normalizeBleAddress(entry?.bleAddress);
    if (!bleAddress) throw new HttpError(400, `Invalid bleAddress in discovered at index ${i}`);
    const name =
      typeof entry?.name === 'string' && entry.name.trim() !== ''
        ? entry.name.trim().slice(0, MAX_ADV_NAME)
        : undefined;
    const rssi =
      Number.isInteger(entry?.rssi) &&
      (entry?.rssi as number) >= -127 &&
      (entry?.rssi as number) <= 20
        ? (entry?.rssi as number)
        : undefined;
    byAddress.set(bleAddress, { bleAddress, name, rssi, seenAt: readTime(entry?.seenAt, now) });
  }
  return [...byAddress.values()];
}

/** Notes cameras heard in a scan; a camera heard for the first time is new. */
export async function recordDiscovered(
  store: Store,
  hubId: string,
  entries: DiscoveredEntry[],
): Promise<void> {
  for (const entry of entries) {
    const known = await store.getFoundCamera(hubId, entry.bleAddress);
    await store.putFoundCamera(hubId, {
      ...(known ?? { state: 'new', firstSeen: entry.seenAt }),
      bleAddress: entry.bleAddress,
      advName: entry.name ?? known?.advName,
      rssi: entry.rssi ?? known?.rssi,
      lastSeen: Math.max(entry.seenAt, known?.lastSeen ?? 0),
    });
  }
}

/**
 * A camera the hub has read, with its BLE address. A camera read before any
 * decision was made about it (existing cameras, hubs that read everything)
 * is approved; a decision already made stays. Returns the name given in the
 * gallery before the first read, if any.
 */
export async function recordRead(
  store: Store,
  hubId: string,
  camId: string,
  bleAddress: string,
  readAt: number,
): Promise<string | undefined> {
  const known = await store.getFoundCamera(hubId, bleAddress);
  const state = known === undefined || known.state === 'new' ? 'approved' : known.state;
  await store.putFoundCamera(hubId, {
    ...known,
    bleAddress,
    state,
    camId,
    firstSeen: known?.firstSeen ?? readAt,
    lastSeen: Math.max(readAt, known?.lastSeen ?? 0),
  });
  return known?.alias;
}

/**
 * BLE addresses the hub may read, sorted. A camera hidden in the gallery
 * (#273) is left out, so the hub stops reading it; showing it again adds it
 * back at the next check-in.
 */
export async function approvedAddresses(store: Store, hubId: string): Promise<string[]> {
  const addresses: string[] = [];
  for (const found of await store.listFoundCameras(hubId)) {
    if (found.state !== 'approved') continue;
    if (found.camId !== undefined && (await store.getCamera(found.camId))?.hidden) continue;
    addresses.push(found.bleAddress);
  }
  return addresses.sort();
}

/** For the gallery: new ones first, then by last seen. */
export function sortForGallery(cameras: FoundCamera[]): FoundCamera[] {
  const rank = { new: 0, approved: 1, excluded: 2 } as const;
  return [...cameras].sort((a, b) => rank[a.state] - rank[b.state] || b.lastSeen - a.lastSeen);
}
