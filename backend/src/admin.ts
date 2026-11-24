import type { Deps } from './api';
import { generateAppToken, newAppUserId } from './auth/app';
import { generateDeviceKey } from './auth/device';
import { generateTotpSecret, totpUri } from './auth/totp';
import { MAX_CAMERA_NAME, validCameraName } from './model';
import type { AppUser, UserNotify } from './model';
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

/**
 * Creates a gallery user (#259) linked to hubs, or with `userId` issues a new
 * token for an existing one (the old token stops working). The token is
 * returned once.
 */
export interface CreateAppUserEvent {
  admin: 'createAppUser';
  name?: string;
  hubIds?: string[];
  role?: 'user' | 'admin';
  userId?: number;
  /** Admin only: issue a new TOTP secret (lost phone); a new admin gets one anyway. */
  rotateTotp?: boolean;
  /** Mail address for hub notices (#445); `null` removes it. */
  email?: string | null;
  /** Notices to mail; missing keys keep their value (default: all on once an address is set). */
  notify?: Partial<UserNotify>;
}

/**
 * Locks a gallery user out, e.g. after a lost laptop: the token stops
 * working at once. `createAppUser` with the same `userId` gives a new one.
 */
export interface RevokeAppUserEvent {
  admin: 'revokeAppUser';
  userId?: number;
}

/** Gives a camera the name the user sees in the gallery. */
export interface RenameCameraEvent {
  admin: 'renameCamera';
  camId?: string;
  name?: string;
}

export type AdminEvent =
  | RegisterHubEvent
  | CreateAppUserEvent
  | RevokeAppUserEvent
  | RenameCameraEvent;

type Failure = { ok: false; error: string };
export type RegisterHubResult =
  | { ok: true; created: boolean; key?: string; linkedUsers: number[] }
  | Failure;
export type AppUserResult =
  | {
      ok: true;
      userId: number;
      token: string;
      linkedHubs: string[];
      /** Only when a TOTP secret was issued now (#259); shown once. */
      totpSecret?: string;
      totpUri?: string;
    }
  | Failure;
export type RenameResult = { ok: true; camId: string; name: string } | Failure;
export type RevokeResult = { ok: true; userId: number; name: string } | Failure;
export type AdminResult = RegisterHubResult | AppUserResult | RevokeResult | RenameResult;

export type AdminDeps = Pick<Deps, 'store' | 'now'>;

const HUB_ID = /^[A-Za-z0-9_-]{1,64}$/;

export function isAdminEvent(event: unknown): event is { admin: unknown } {
  return (
    typeof event === 'object' && event !== null && !('requestContext' in event) && 'admin' in event
  );
}

export async function handleAdmin(
  event: { admin: unknown },
  deps: AdminDeps,
): Promise<AdminResult> {
  if (event.admin === 'registerHub') return registerHub(event as RegisterHubEvent, deps.store);
  if (event.admin === 'renameCamera') {
    return renameCamera(event as RenameCameraEvent, deps);
  }
  if (event.admin === 'revokeAppUser') {
    return revokeAppUser(event as RevokeAppUserEvent, deps);
  }
  if (event.admin === 'createAppUser') {
    return createAppUser(event as CreateAppUserEvent, deps);
  }
  return { ok: false, error: `unknown admin task ${JSON.stringify(event.admin)}` };
}

/**
 * Creates a hub (with a new key) or updates an existing one: `name` renames
 * it, `rotateKey` issues a new key, `users` links gallery user ids. The key is
 * returned only when it was created now; only its hash is stored.
 */
export async function registerHub(
  event: RegisterHubEvent,
  store: Store,
): Promise<RegisterHubResult> {
  const { hubId, name, rotateKey = false } = event;
  const users = event.users ?? [];
  if (typeof hubId !== 'string' || !HUB_ID.test(hubId)) {
    return { ok: false, error: 'hubId must be 1-64 characters [A-Za-z0-9_-]' };
  }
  if (name !== undefined && (typeof name !== 'string' || name.trim() === '')) {
    return { ok: false, error: 'name must be a non-empty string' };
  }
  if (!Array.isArray(users) || !users.every((u) => Number.isSafeInteger(u) && u > 0)) {
    return { ok: false, error: 'users must be positive numeric user ids' };
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

export async function createAppUser(
  event: CreateAppUserEvent,
  deps: AdminDeps,
): Promise<AppUserResult> {
  const hubIds = event.hubIds ?? [];
  const role = event.role ?? 'user';
  if (role !== 'user' && role !== 'admin')
    return { ok: false, error: 'role must be user or admin' };
  if (!Array.isArray(hubIds) || !hubIds.every((h) => typeof h === 'string' && HUB_ID.test(h))) {
    return { ok: false, error: 'hubIds must be hub ids' };
  }
  for (const hubId of hubIds) {
    if (!(await deps.store.getHub(hubId))) return { ok: false, error: `unknown hub ${hubId}` };
  }

  const existing =
    event.userId === undefined ? undefined : await deps.store.getAppUser(event.userId);
  if (event.userId !== undefined && !existing) {
    return { ok: false, error: `unknown user ${event.userId}` };
  }
  const name = event.name?.trim() || existing?.name;
  if (!name) return { ok: false, error: 'name must be a non-empty string' };

  const userId = existing?.userId ?? newAppUserId();
  const { token, tokenHash } = generateAppToken(userId);
  const finalRole = event.role ?? existing?.role ?? role;
  if (event.rotateTotp && finalRole !== 'admin') {
    return { ok: false, error: 'only an admin has a TOTP secret' };
  }
  // An admin gets a TOTP secret when created, or when asked for a new one.
  const newSecret =
    finalRole === 'admin' && (event.rotateTotp || !existing?.totpSecret)
      ? generateTotpSecret()
      : undefined;
  const mail = mailSettings(event, existing);
  if (!mail.ok) return mail;
  const user: AppUser = {
    ...existing,
    userId,
    name,
    role: finalRole,
    tokenHash,
    created: existing?.created ?? deps.now(),
  };
  if (mail.email === undefined) {
    delete user.email;
    delete user.notify;
  } else {
    user.email = mail.email;
    user.notify = mail.notify;
  }
  if (newSecret !== undefined) {
    user.totpSecret = newSecret;
    delete user.totpLastStep;
    delete user.totpFailures;
    delete user.totpLockedUntil;
  }
  await deps.store.putAppUser(user);
  for (const hubId of hubIds) await deps.store.linkUser(userId, hubId);
  return {
    ok: true,
    userId,
    token,
    linkedHubs: hubIds,
    ...(newSecret === undefined
      ? {}
      : { totpSecret: newSecret, totpUri: totpUri(newSecret, name) }),
  };
}

const EMAIL = /^[^\s@<>",;]{1,64}@[^\s@<>",;]{1,255}\.[^\s@<>",;]{2,}$/;

/** The address and switches after an admin task (#445). */
function mailSettings(
  event: CreateAppUserEvent,
  existing: AppUser | undefined,
): { ok: true; email?: string; notify?: UserNotify } | Failure {
  let email = existing?.email;
  if (event.email === null) email = undefined;
  else if (event.email !== undefined) {
    const value = typeof event.email === 'string' ? event.email.trim() : '';
    if (!EMAIL.test(value)) return { ok: false, error: 'email must be a mail address' };
    email = value;
  }
  if (email === undefined) return { ok: true };
  const notify: UserNotify = {
    hubSilent: true,
    cameraNotRead: true,
    battery: true,
    ...existing?.notify,
  };
  for (const key of Object.keys(event.notify ?? {}) as Array<keyof UserNotify>) {
    const value = event.notify?.[key];
    if (!(key in notify) || typeof value !== 'boolean') {
      return { ok: false, error: 'notify must hold hubSilent, cameraNotRead, battery as booleans' };
    }
    notify[key] = value;
  }
  return { ok: true, email, notify };
}

export async function revokeAppUser(
  event: RevokeAppUserEvent,
  deps: AdminDeps,
): Promise<RevokeResult> {
  const id = event.userId;
  const user = Number.isSafeInteger(id) ? await deps.store.getAppUser(id as number) : undefined;
  if (!user) return { ok: false, error: `unknown user ${event.userId}` };
  // An empty hash matches no token; the hub links stay for a new token.
  await deps.store.putAppUser({ ...user, tokenHash: '' });
  return { ok: true, userId: user.userId, name: user.name };
}

export async function renameCamera(
  event: RenameCameraEvent,
  deps: AdminDeps,
): Promise<RenameResult> {
  const camId = event.camId ?? '';
  const name = validCameraName(event.name);
  if (name === undefined) {
    return { ok: false, error: `name must be 1-${MAX_CAMERA_NAME} printable characters` };
  }
  if (!(await deps.store.getCamera(camId))) return { ok: false, error: `unknown camera ${camId}` };
  await deps.store.renameCamera(camId, name);
  return { ok: true, camId, name };
}
