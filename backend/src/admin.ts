import { generateDeviceKey } from './auth/device';
import type { Store } from './store/types';

/**
 * Administrative tasks, run by invoking the Lambda function directly
 * (`aws lambda invoke`). They are not reachable through the Function URL:
 * every URL event carries a `requestContext`, and only events without one are
 * treated as admin events. Invoking the function needs IAM permission
 * (lambda:InvokeFunction), so only account admins can use them.
 *
 * This exists because AWS projects deny people direct access to DynamoDB
 * data through a service control policy, while the function's own role may
 * still read and write the table.
 */
export interface RegisterHubEvent {
  admin: 'registerHub';
  hubId: string;
  name?: string;
  users?: number[];
  rotateKey?: boolean;
}

export type AdminEvent = RegisterHubEvent;

export type AdminResult =
  | { ok: true; created: boolean; key?: string; linkedUsers: number[] }
  | { ok: false; error: string };

const HUB_ID = /^[A-Za-z0-9_-]{1,64}$/;

export function isAdminEvent(event: unknown): event is { admin: unknown } {
  return (
    typeof event === 'object' && event !== null && !('requestContext' in event) && 'admin' in event
  );
}

export async function handleAdmin(event: { admin: unknown }, store: Store): Promise<AdminResult> {
  if (event.admin !== 'registerHub') {
    return { ok: false, error: `unknown admin task ${JSON.stringify(event.admin)}` };
  }
  return registerHub(event as RegisterHubEvent, store);
}

/**
 * Creates a hub (with a new key) or updates an existing one: `name` renames
 * it, `rotateKey` issues a new key, `users` links Telegram users. The key is
 * returned only when it was created now; only its hash is stored.
 */
export async function registerHub(event: RegisterHubEvent, store: Store): Promise<AdminResult> {
  const { hubId, name, rotateKey = false } = event;
  const users = event.users ?? [];
  if (typeof hubId !== 'string' || !HUB_ID.test(hubId)) {
    return { ok: false, error: 'hubId must be 1-64 characters [A-Za-z0-9_-]' };
  }
  if (name !== undefined && (typeof name !== 'string' || name.trim() === '')) {
    return { ok: false, error: 'name must be a non-empty string' };
  }
  if (!Array.isArray(users) || !users.every((u) => Number.isSafeInteger(u) && u > 0)) {
    return { ok: false, error: 'users must be positive numeric Telegram user ids' };
  }

  const existing = await store.getHub(hubId);
  let key: string | undefined;
  if (!existing || rotateKey || name !== undefined) {
    let keyHash = existing?.keyHash;
    if (!existing || rotateKey) {
      const generated = generateDeviceKey(hubId);
      key = generated.key;
      keyHash = generated.keyHash;
    }
    await store.putHub({
      ...existing,
      hubId,
      name: name?.trim() ?? existing?.name ?? hubId,
      keyHash: keyHash as string,
      lastSeen: existing?.lastSeen ?? 0,
    });
  }
  for (const user of users) {
    await store.linkUser(user, hubId);
  }
  return { ok: true, created: !existing, key, linkedUsers: users };
}
