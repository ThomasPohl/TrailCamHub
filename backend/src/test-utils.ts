import { generateDeviceKey } from './auth/device';
import { signInitData } from './auth/telegram';
import type { Deps } from './api';
import type { ApiRequest } from './http';
import type { MediaStorage } from './storage';
import { MemoryStore } from './store/memory';

export const BOT_TOKEN = '123456:TEST-token-for-unit-tests';
export const NOW = 1_800_000_000;
export const USER_ID = 4242;
export const HUB_ID = 'garden-hub';
export const CAM_ID = 'F4C15606ED3B';
export const CAM_2 = 'A1B2C3D4E5F6';

/** Builds init data the way Telegram does, signed with BOT_TOKEN. */
export function makeInitData(userId = USER_ID, authDate = NOW - 60, botToken = BOT_TOKEN): string {
  const params = new URLSearchParams({
    query_id: 'AAH_test',
    user: JSON.stringify({ id: userId, first_name: 'Thomas' }),
    auth_date: String(authDate),
  });
  params.set('hash', signInitData(params, botToken));
  return params.toString();
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
}

/** A store with one hub linked to USER_ID, no cameras yet. */
export async function setup() {
  const store = new MemoryStore();
  const storage = new FakeStorage();
  const hub = generateDeviceKey(HUB_ID);
  await store.putHub({ hubId: HUB_ID, name: 'Garden hub', keyHash: hub.keyHash, lastSeen: 0 });
  await store.linkUser(USER_ID, HUB_ID);
  const other = generateDeviceKey('other-hub');
  await store.putHub({ hubId: 'other-hub', name: 'Other', keyHash: other.keyHash, lastSeen: 0 });
  let nextId = 1;
  const deps: Deps = {
    store,
    storage,
    botToken: async () => BOT_TOKEN,
    now: () => NOW,
    newId: () => `cmd-${nextId++}`,
  };
  return { store, storage, deps, hubKey: hub.key, otherHubKey: other.key };
}

export function appRequest(
  method: string,
  path: string,
  body?: unknown,
  initData = makeInitData(),
): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `tma ${initData}` },
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
