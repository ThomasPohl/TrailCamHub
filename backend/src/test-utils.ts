import { generateDeviceKey, hashSecret } from './auth/device';
import type { Deps } from './api';
import type { ApiRequest } from './http';
import type { MediaStorage } from './storage';
import { MemoryStore } from './store/memory';

export const NOW = 1_800_000_000;
/** A gallery user (app user ids start at 10^15, #259) linked to HUB_ID. */
export const USER_ID = 1_000_000_000_004_242;
export const HUB_ID = 'garden-hub';
export const CAM_ID = 'F4C15606ED3B';
export const CAM_2 = 'A1B2C3D4E5F6';

const TEST_SECRET = 'test-secret-0123456789abcdef';

/** The app token of a test user (all test users share one secret). */
export function userToken(userId = USER_ID): string {
  return `${userId}.${TEST_SECRET}`;
}

export class FakeStorage implements MediaStorage {
  readonly uploaded = new Set<string>();
  async thumbReadUrl(camId: string, mediaId: number) {
    return `https://media.test/get/${camId}/${mediaId}.jpg`;
  }
  async thumbUploadUrl(camId: string, mediaId: number) {
    return `https://media.test/put/${camId}/${mediaId}.jpg`;
  }
  async thumbExists(camId: string, mediaId: number) {
    return this.uploaded.has(`${camId}/${mediaId}`);
  }
  readonly uploadedVideos = new Set<string>();
  async videoReadUrl(camId: string, mediaId: number) {
    return `https://media.test/get/${camId}/${mediaId}.mp4`;
  }
  async videoUploadUrl(camId: string, mediaId: number) {
    return `https://media.test/put/${camId}/${mediaId}.mp4`;
  }
  async videoExists(camId: string, mediaId: number) {
    return this.uploadedVideos.has(`${camId}/${mediaId}`);
  }
}

/** A store with one hub linked to USER_ID, no cameras yet. */
export async function setup() {
  const store = new MemoryStore();
  const storage = new FakeStorage();
  const hub = generateDeviceKey(HUB_ID);
  await store.putHub({ hubId: HUB_ID, name: 'Garden hub', keyHash: hub.keyHash, lastSeen: 0 });
  await store.linkUser(USER_ID, HUB_ID);
  // Two users with a token: USER_ID (linked to the hub) and USER_ID + 1 (not).
  for (const userId of [USER_ID, USER_ID + 1]) {
    await store.putAppUser({
      userId,
      name: 'Test',
      role: 'user',
      tokenHash: hashSecret(TEST_SECRET),
      created: NOW,
    });
  }
  const other = generateDeviceKey('other-hub');
  await store.putHub({ hubId: 'other-hub', name: 'Other', keyHash: other.keyHash, lastSeen: 0 });
  let nextId = 1;
  const deps: Deps = {
    store,
    storage,
    now: () => NOW,
    newId: () => `cmd-${nextId++}`,
  };
  return { store, storage, deps, hubKey: hub.key, otherHubKey: other.key };
}

export function appRequest(
  method: string,
  path: string,
  body?: unknown,
  token = userToken(),
): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `Bearer ${token}` },
    body: body === undefined ? undefined : JSON.stringify(body),
  };
}

export function hubRequest(path: string, hubKey: string, body: unknown): ApiRequest {
  return {
    method: 'POST',
    path,
    headers: { authorization: `Bearer ${hubKey}` },
    body: JSON.stringify(body),
  };
}
