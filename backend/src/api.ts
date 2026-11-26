import { randomUUID } from 'node:crypto';
import {
  ADMIN_SESSION_S,
  createAdminSession,
  parseAdminSession,
  verifyAdminSession,
} from './auth/admin-session';
import { parseAppToken } from './auth/app';
import { parseDeviceKey, verifySecret } from './auth/device';
import { verifyTotp } from './auth/totp';
import { createAppUser, revokeAppUser } from './admin';
import {
  deleteInBackend,
  deleteTargetsOf,
  hubDeletesByTarget,
  parseDeleteTargets,
  requestDelete,
  requestSettings,
  requestVideo,
  targetsOf,
  withdraw,
  withdrawSettings,
} from './commands';
import { parseCapability, parseSettingsReport } from './settings';
import {
  HttpError,
  json,
  matchPath,
  parseJsonBody,
  type ApiRequest,
  type ApiResponse,
} from './http';
import type {
  AckRequest,
  AdminHub,
  AppUser,
  Camera,
  CameraCheckin,
  CameraRecord,
  DataUsageReport,
  HubUsage,
  CameraReport,
  CheckinRequest,
  CheckinResponse,
  CommandRecord,
  CommandResult,
  FirmwareChange,
  FirmwareUpdateResult,
  HubRecord,
  HubMemory,
  HubPowerMode,
  PowerSetting,
  HubUplink,
  HubHistory,
  HubSchedule,
  MediaItem,
  MediaRecord,
  Sample,
  SampleOwner,
  ScheduleConfig,
  ScheduleOrigin,
  ScheduleStretch,
  SdCardState,
} from './model';
import { hubFaults, userFaults } from './faults';
import { loadHubInputs } from './hub-inputs';
import {
  approvedAddresses,
  parseDiscovered,
  recordDiscovered,
  recordRead,
  sortForGallery,
} from './found-cameras';
import {
  ID_PATTERN,
  isMediaId,
  MAX_CAMERA_NAME,
  MAX_DATA_BUDGET_MB,
  normalizeBleAddress,
  POWER_SETTINGS,
  validCameraName,
} from './model';
import {
  formatPlan,
  MAX_CHECKIN_INTERVAL_S,
  MAX_PLAN_LENGTH,
  MIN_CHECKIN_INTERVAL_S,
  parsePlan,
} from './schedule';
import type { MediaStorage } from './storage';
import type { CameraTelemetry, Store } from './store/types';
import { createShare, listShares, revokeShare, showShare } from './share';

export interface Deps {
  store: Store;
  storage: MediaStorage;
  now: () => number;
  newId?: () => string;
  config?: {
    /** Thumbnail upload URLs handed out per check-in, over all cameras. */
    maxThumbUploadsPerCheckin?: number;
  };
}

const DEFAULT_MAX_THUMB_UPLOADS = 30;
/**
 * Delete commands handed out per camera and check-in (#261). A gallery batch
 * can hold 200; the hub queues at most 40, and a hub without PSRAM must keep
 * the response small (#244). The rest follows with the next check-ins.
 */
const MAX_DELETES_PER_CHECKIN = 20;
const MAX_CAMERAS_PER_CHECKIN = 16;
const MAX_MEDIA_PER_CHECKIN = 2_000;
/** A hub clock may lag; readAt values further in the future are ignored. */
const MAX_CLOCK_SKEW_S = 5 * 60;
const MAX_READ_AGE_S = 30 * 24 * 60 * 60;
/** Planned times further ahead than this are not believed. */
const MAX_PLAN_AHEAD_S = 8 * 24 * 60 * 60;

type Handler = (
  req: ApiRequest,
  params: Record<string, string>,
  deps: Deps,
) => Promise<ApiResponse>;

const routes: Array<{ method: string; pattern: string; handler: Handler }> = [
  { method: 'GET', pattern: '/v1/health', handler: async () => json(200, { ok: true }) },
  { method: 'GET', pattern: '/v1/me', handler: me },
  { method: 'GET', pattern: '/v1/cameras', handler: listCameras },
  { method: 'GET', pattern: '/v1/cameras/:camId/media', handler: listMedia },
  { method: 'POST', pattern: '/v1/cameras/:camId/commands', handler: createCommand },
  { method: 'POST', pattern: '/v1/cameras/:camId/media/:mediaId/withdraw', handler: withdrawMedia },
  { method: 'POST', pattern: '/v1/cameras/:camId/media/:mediaId/share', handler: shareMedia },
  { method: 'GET', pattern: '/v1/shares', handler: listOwnShares },
  // POST, not DELETE: the Function URL's CORS allows only GET and POST.
  { method: 'POST', pattern: '/v1/shares/:shareId/revoke', handler: revokeOwnShare },
  // Public: opens without a token (#444); the token in the path is the secret.
  {
    method: 'GET',
    pattern: '/v1/s/:token',
    handler: async (_r, p, d) => showShare(p.token ?? '', d),
  },
  { method: 'POST', pattern: '/v1/cameras/:camId/name', handler: renameCameraByUser },
  { method: 'POST', pattern: '/v1/cameras/:camId/hidden', handler: hideCamera },
  {
    method: 'POST',
    pattern: '/v1/cameras/:camId/settings/withdraw',
    handler: withdrawCameraSettings,
  },
  { method: 'PUT', pattern: '/v1/hubs/:hubId/schedule', handler: setSchedule },
  // The same as POST: the Function URL's CORS allows no PUT (#286).
  { method: 'POST', pattern: '/v1/hubs/:hubId/schedule', handler: setSchedule },
  { method: 'POST', pattern: '/v1/hubs/:hubId/delete-targets', handler: setDeleteTargets },
  { method: 'GET', pattern: '/v1/hubs/:hubId/found-cameras', handler: listFoundCameras },
  { method: 'POST', pattern: '/v1/hubs/:hubId/scan', handler: requestScan },
  {
    method: 'POST',
    pattern: '/v1/hubs/:hubId/found-cameras/:address',
    handler: decideFoundCamera,
  },
  // What the user of a hub may see of its history and data volume, and the
  // hints in plain words (#431); the technical faults stay with the admin.
  { method: 'GET', pattern: '/v1/hubs/:hubId/history', handler: hubHistory },
  { method: 'GET', pattern: '/v1/hubs/:hubId/usage', handler: hubUsage },
  { method: 'GET', pattern: '/v1/hubs/:hubId/hints', handler: hubHints },
  { method: 'GET', pattern: '/v1/admin/hubs', handler: adminHubs },
  { method: 'GET', pattern: '/v1/admin/hubs/:hubId/history', handler: adminHistory },
  { method: 'GET', pattern: '/v1/admin/hubs/:hubId/usage', handler: adminUsage },
  { method: 'POST', pattern: '/v1/admin/hubs/:hubId/data-budget', handler: setDataBudget },
  { method: 'POST', pattern: '/v1/admin/elevate', handler: elevate },
  { method: 'POST', pattern: '/v1/admin/users/:userId/rotate', handler: rotateUser },
  { method: 'POST', pattern: '/v1/admin/users/:userId/revoke', handler: revokeUser },
  { method: 'GET', pattern: '/v1/admin/audit', handler: listAudit },
  { method: 'POST', pattern: '/v1/device/checkin', handler: checkin },
  { method: 'POST', pattern: '/v1/device/ack', handler: ack },
];

export async function handleRequest(req: ApiRequest, deps: Deps): Promise<ApiResponse> {
  try {
    let pathMatched = false;
    for (const route of routes) {
      const params = matchPath(route.pattern, req.path);
      if (!params) continue;
      pathMatched = true;
      if (route.method === req.method) return await route.handler(req, params, deps);
    }
    throw new HttpError(pathMatched ? 405 : 404, pathMatched ? 'Method not allowed' : 'Not found');
  } catch (error) {
    if (error instanceof HttpError) return json(error.statusCode, { error: error.message });
    console.error('Unhandled error', error);
    return json(500, { error: 'Internal error' });
  }
}

// ---- Authentication ----------------------------------------------------------

function bearer(req: ApiRequest, scheme: string): string {
  const header = req.headers.authorization ?? '';
  const prefix = `${scheme} `;
  if (!header.startsWith(prefix)) throw new HttpError(401, `Expected Authorization: ${scheme} …`);
  return header.slice(prefix.length).trim();
}

/**
 * Who is signed in. Only gallery users can be admins (#259). `admin` means the
 * admin's powers are active: the request carries a valid admin session. An
 * admin with a plain app token (`role` admin, `admin` false) acts like a user.
 */
interface SignedIn {
  userId: number;
  name?: string;
  role: 'user' | 'admin';
  admin: boolean;
}

/** Wrong TOTP codes in a row before the account is locked, and for how long. */
const MAX_TOTP_FAILURES = 5;
const TOTP_LOCK_S = 15 * 60;

/**
 * The user behind a plain app token (`Bearer <userId>.<secret>`). A revoked
 * token (empty hash) never matches.
 */
async function bearerUser(req: ApiRequest, deps: Deps): Promise<AppUser> {
  const parsed = parseAppToken(bearer(req, 'Bearer'));
  const user = parsed && (await deps.store.getAppUser(parsed.userId));
  if (!parsed || !user || !user.tokenHash || !verifySecret(parsed.secret, user.tokenHash)) {
    throw new HttpError(401, 'Invalid app token');
  }
  return user;
}

/**
 * The signed-in user: a gallery user with an app token (`Bearer
 * <userId>.<secret>`, #259), or an admin with a session from `POST
 * /v1/admin/elevate` (`Admin <session>`). The session replaces the token for
 * the time it lasts and ends as soon as the token or the TOTP secret changes.
 */
async function authUser(req: ApiRequest, deps: Deps): Promise<SignedIn> {
  const header = req.headers.authorization ?? '';
  if (header.startsWith('Admin ')) {
    const parsed = parseAdminSession(header.slice('Admin '.length).trim());
    const user = parsed && (await deps.store.getAppUser(parsed.userId));
    if (!parsed || !user || !verifyAdminSession(user, parsed, deps.now())) {
      throw new HttpError(401, 'Invalid or expired admin session');
    }
    return { userId: user.userId, name: user.name, role: 'admin', admin: true };
  }
  const user = await bearerUser(req, deps);
  return { userId: user.userId, name: user.name, role: user.role, admin: false };
}

/** Only with an active admin session; an admin without one is told to elevate. */
function requireAdmin(user: SignedIn): void {
  if (user.admin) return;
  throw new HttpError(403, user.role === 'admin' ? 'Admin session required' : 'Only for the admin');
}

/** The admin sees every hub; everyone else the hubs linked to him. */
async function mayUseHub(user: SignedIn, hubId: string, deps: Deps): Promise<boolean> {
  return user.admin || deps.store.isLinked(user.userId, hubId);
}

async function authUserForCamera(
  req: ApiRequest,
  camId: string,
  deps: Deps,
): Promise<CameraRecord> {
  const user = await authUser(req, deps);
  const camera = await deps.store.getCamera(camId);
  // Same answer for "unknown" and "not yours", so camera ids cannot be probed.
  if (!camera || !(await mayUseHub(user, camera.hubId, deps))) {
    throw new HttpError(404, 'Camera not found');
  }
  return camera;
}

async function authHub(req: ApiRequest, deps: Deps): Promise<HubRecord> {
  const parsed = parseDeviceKey(bearer(req, 'Bearer'));
  const hub = parsed && (await deps.store.getHub(parsed.id));
  if (!parsed || !hub || !verifySecret(parsed.secret, hub.keyHash)) {
    throw new HttpError(401, 'Invalid hub key');
  }
  return hub;
}

// ---- App endpoints -------------------------------------------------------------

function isOpen(c: CommandRecord): boolean {
  return c.state === 'queued' || c.state === 'delivered';
}

function toCamera(
  record: CameraRecord,
  hub: HubRecord | undefined,
  commands: CommandRecord[],
): Camera {
  return {
    camId: record.camId,
    name: record.name,
    hidden: record.hidden || undefined,
    hubId: record.hubId,
    hubName: hub?.name ?? record.hubId,
    model: record.model ?? 'unknown',
    firmware: record.firmware ?? 'unknown',
    batPercent: record.batPercent ?? 0,
    sdFreeMB: record.sdFreeMB ?? 0,
    sdTotalMB: record.sdTotalMB,
    sdCard: record.sdCard ?? 'unknown',
    sdError: record.sdCard === 'missing' && record.sdError ? record.sdError : undefined,
    temperature: record.temperature,
    isCharging: record.isCharging,
    isSolarCharging: record.isSolarCharging,
    batType: record.batType,
    bleVersion: record.bleVersion,
    cameraName: record.cameraName,
    hubBatPercent: hub?.batPercent,
    lastSeen: record.lastSeen,
    hubLastSeen: hub?.lastSeen ?? 0,
    hubSchedule: hub?.schedule,
    hubScheduleConfig: hub?.scheduleConfig ?? {},
    hubDeleteTargets: deleteTargetsOf(hub),
    hubDeletesByTarget: hubDeletesByTarget(hub),
    readRequested: commands.some((c) => c.type === 'readCamera' && isOpen(c)),
    settings: record.settings,
    capability: record.capability,
    ...settingsState(commands),
  };
}

/**
 * The open settings changes of a camera, merged in the order they were asked
 * for, or the error of the last one if it failed (#368).
 */
function settingsState(
  commands: CommandRecord[],
): Pick<Camera, 'settingsPending' | 'settingsState' | 'settingsError'> {
  const mine = commands
    .filter((c) => c.type === 'setSettings')
    .sort((a, b) => a.created - b.created);
  const open = mine.filter(isOpen);
  if (open.length > 0) {
    return {
      settingsPending: Object.assign({}, ...open.map((c) => c.settings ?? {})),
      settingsState: open.some((c) => c.state === 'delivered') ? 'delivered' : 'requested',
    };
  }
  const last = mine.filter((c) => c.state !== 'cancelled').at(-1);
  return last?.state === 'failed' ? { settingsError: last.error ?? 'failed on the hub' } : {};
}

/** Who is signed in, so the gallery can offer the admin view (#264). */
async function me(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  // The hubs, also those without a camera yet: the gallery asks each for the
  // cameras it found (#298).
  const hubIds = user.admin
    ? await deps.store.listHubIds()
    : await deps.store.listUserHubIds(user.userId);
  const hubs = [];
  for (const hubId of hubIds) {
    const hub = await deps.store.getHub(hubId);
    if (hub) hubs.push({ hubId, name: hub.name });
  }
  return json(200, {
    userId: user.userId,
    name: user.name ?? '',
    role: user.role,
    elevated: user.admin,
    hubs,
  });
}

/** Every hub with its cameras and users, for the admin view (#264). */
async function adminHubs(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  requireAdmin(user);
  const hubs: AdminHub[] = [];
  for (const hubId of await deps.store.listHubIds()) {
    const hub = await deps.store.getHub(hubId);
    if (!hub) continue;
    const { records, firmwareChanges, excluded } = await loadHubInputs(deps.store, hub);
    const cameras = records
      .map((r) => toCamera(r.camera, hub, r.commands))
      .sort((a, b) => a.name.localeCompare(b.name));
    const faults = hubFaults(hub, records, deps.now(), firmwareChanges, excluded).sort(
      (a, b) => Number(a.level === 'warning') - Number(b.level === 'warning'),
    );
    const users = await Promise.all(
      (await deps.store.listHubUserIds(hubId)).map(async (userId) => {
        const app = await deps.store.getAppUser(userId);
        return app ? { userId, name: app.name, role: app.role } : { userId };
      }),
    );
    hubs.push({
      hubId,
      name: hub.name,
      firmware: hub.firmware,
      batPercent: hub.batPercent,
      powerMode: hub.powerMode,
      uplink: hub.uplink,
      modemSignalDbm: hub.uplink === 'modem' ? hub.modemSignalDbm : undefined,
      modemOperator: hub.uplink === 'modem' ? hub.modemOperator : undefined,
      memory: hub.memory,
      dataBudgetMB: hub.dataBudgetMB,
      lastSeen: hub.lastSeen,
      schedule: hub.schedule,
      scheduleConfig: hub.scheduleConfig ?? {},
      cameras,
      users,
      faults,
      firmwareChanges,
    });
  }
  return json(200, hubs);
}

/**
 * Exchanges a TOTP code for an admin session (#259). Takes the plain app token
 * (not a session), counts wrong codes and locks the account for a while after
 * too many. A code works once.
 */
async function elevate(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const user = await bearerUser(req, deps);
  if (user.role !== 'admin' || !user.totpSecret) {
    throw new HttpError(403, 'No second factor set up for this user');
  }
  const now = deps.now();
  if ((user.totpLockedUntil ?? 0) > now) {
    throw new HttpError(429, 'Too many wrong codes, try again later');
  }
  const { code } = parseJsonBody<{ code?: unknown }>(req);
  const step =
    typeof code === 'string'
      ? verifyTotp(user.totpSecret, code, now, user.totpLastStep)
      : undefined;
  if (step === undefined) {
    const failures = (user.totpFailures ?? 0) + 1;
    const locked = failures >= MAX_TOTP_FAILURES;
    await deps.store.putAppUser({
      ...user,
      totpFailures: locked ? 0 : failures,
      ...(locked ? { totpLockedUntil: now + TOTP_LOCK_S } : {}),
    });
    await deps.store.addAudit({
      at: now,
      actor: user.userId,
      action: locked ? 'elevateLocked' : 'elevateFailed',
    });
    throw new HttpError(401, 'Wrong code');
  }
  const { totpLockedUntil: _lock, ...rest } = user;
  void _lock;
  await deps.store.putAppUser({ ...rest, totpLastStep: step, totpFailures: 0 });
  await deps.store.addAudit({ at: now, actor: user.userId, action: 'elevate' });
  return json(200, { session: createAdminSession(user, now), expiresAt: now + ADMIN_SESSION_S });
}

/** The user an admin acts on; admin accounts are never changed through the API. */
async function targetUser(params: Record<string, string>, deps: Deps): Promise<AppUser> {
  const id = Number(params.userId);
  const target = Number.isSafeInteger(id) ? await deps.store.getAppUser(id) : undefined;
  if (!target) throw new HttpError(404, 'User not found');
  if (target.role === 'admin') throw new HttpError(403, 'Admin accounts are managed with add-user');
  return target;
}

/** New token for a user; the old link stops working. The token is returned once. */
async function rotateUser(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const admin = await authUser(req, deps);
  requireAdmin(admin);
  const target = await targetUser(params, deps);
  const result = await createAppUser({ admin: 'createAppUser', userId: target.userId }, deps);
  if (!result.ok) throw new HttpError(500, result.error);
  await deps.store.addAudit({
    at: deps.now(),
    actor: admin.userId,
    action: 'rotateUser',
    target: target.userId,
  });
  return json(200, { userId: target.userId, name: target.name, token: result.token });
}

/** Locks a user out; the hub links stay, so rotating gives a working link again. */
async function revokeUser(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const admin = await authUser(req, deps);
  requireAdmin(admin);
  const target = await targetUser(params, deps);
  const result = await revokeAppUser({ admin: 'revokeAppUser', userId: target.userId }, deps);
  if (!result.ok) throw new HttpError(500, result.error);
  await deps.store.addAudit({
    at: deps.now(),
    actor: admin.userId,
    action: 'revokeUser',
    target: target.userId,
  });
  return json(200, { userId: target.userId, name: target.name });
}

const AUDIT_LIMIT = 50;

async function listAudit(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  requireAdmin(await authUser(req, deps));
  return json(200, await deps.store.listAudit(AUDIT_LIMIT));
}

/** How far back the admin view shows the battery history (#76). */
const HISTORY_VIEW_S = 30 * 24 * 60 * 60;

/** Battery and SD samples of a hub and its cameras (#76). */
async function historyOf(hubId: string, deps: Deps): Promise<HubHistory> {
  const since = deps.now() - HISTORY_VIEW_S;
  const history: HubHistory = {
    hub: await deps.store.listSamples({ hubId }, since),
    cameras: [],
  };
  for (const camId of await deps.store.listHubCameraIds(hubId)) {
    const camera = await deps.store.getCamera(camId);
    if (!camera || camera.hubId !== hubId) continue;
    history.cameras.push({
      camId,
      name: camera.name,
      samples: await deps.store.listSamples({ camId }, since),
    });
  }
  return history;
}

/** For the admin: any hub. */
async function adminHistory(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  requireAdmin(user);
  const hubId = params.hubId ?? '';
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  if (!hub) throw new HttpError(404, 'Hub not found');
  return json(200, await historyOf(hubId, deps));
}

/** For every user linked to the hub (#431). */
async function hubHistory(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  return json(200, await historyOf(hub.hubId, deps));
}

/** UTC day of a Unix time, `YYYY-MM-DD`. */
function utcDay(seconds: number): string {
  return new Date(seconds * 1000).toISOString().slice(0, 10);
}

/**
 * The day sums of the previous and the current month, with a day of slack on
 * both sides so that the app can assign days to the month of its own time zone.
 */
async function usageOf(hub: HubRecord, deps: Deps): Promise<HubUsage> {
  const now = new Date(deps.now() * 1000);
  const previousMonth = Date.UTC(now.getUTCFullYear(), now.getUTCMonth() - 1, 1) / 1000;
  return {
    budgetMB: hub.dataBudgetMB,
    days: await deps.store.listDataUsage(hub.hubId, utcDay(previousMonth - 24 * 3600)),
  };
}

async function adminUsage(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  requireAdmin(await authUser(req, deps));
  const hubId = params.hubId ?? '';
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  if (!hub) throw new HttpError(404, 'Hub not found');
  return json(200, await usageOf(hub, deps));
}

/** For every user linked to the hub, read only; the plan is the admin's (#431). */
async function hubUsage(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  return json(200, await usageOf(hub, deps));
}

/**
 * What needs a look at a hub, as far as the user can do something about it:
 * the hub not reporting, batteries, full SD cards, a camera not read. The
 * technical faults (failed requests, firmware, memory) are the admin's (#431).
 */
async function hubHints(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  const { records, firmwareChanges, excluded } = await loadHubInputs(deps.store, hub);
  const faults = userFaults(hubFaults(hub, records, deps.now(), firmwareChanges, excluded));
  return json(200, faults);
}

/** Sets or removes the monthly data plan of a hub (#415). */
async function setDataBudget(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  requireAdmin(await authUser(req, deps));
  const hubId = params.hubId ?? '';
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  if (!hub) throw new HttpError(404, 'Hub not found');
  const value = parseJsonBody<{ budgetMB?: unknown } | null>(req)?.budgetMB;
  if (value === null || value === undefined) {
    await deps.store.setHubDataBudget(hubId, undefined);
    return json(200, { hubId });
  }
  if (!Number.isInteger(value) || (value as number) < 1 || (value as number) > MAX_DATA_BUDGET_MB) {
    throw new HttpError(400, `budgetMB must be a whole number of 1-${MAX_DATA_BUDGET_MB}, or null`);
  }
  await deps.store.setHubDataBudget(hubId, value as number);
  return json(200, { hubId, budgetMB: value });
}

async function listCameras(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  const hubIds = user.admin
    ? await deps.store.listHubIds()
    : await deps.store.listUserHubIds(user.userId);
  const cameras: Camera[] = [];
  for (const hubId of hubIds) {
    const hub = await deps.store.getHub(hubId);
    const ids = await deps.store.listHubCameraIds(hubId);
    const records = await Promise.all(ids.map((id) => deps.store.getCamera(id)));
    for (const record of records) {
      // A camera listed under a hub it no longer belongs to is skipped.
      if (record && record.hubId === hubId) {
        const commands = await deps.store.listCommands(record.camId);
        cameras.push(toCamera(record, hub, commands));
      }
    }
  }
  // Most recently read first.
  cameras.sort((a, b) => b.lastSeen - a.lastSeen || a.name.localeCompare(b.name));
  return json(200, cameras);
}

async function listMedia(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  // The latest command of each kind per recording (commands sort by creation).
  const commands = [...(await deps.store.listCommands(camera.camId))].sort(
    (a, b) => a.created - b.created,
  );
  const latest = new Map<string, CommandRecord>();
  for (const c of commands) {
    if (c.type !== 'readCamera' && c.state !== 'cancelled') latest.set(`${c.type}:${c.mediaId}`, c);
  }
  const media = (await deps.store.listMedia(camera.camId))
    // Deleted in the gallery stays as a tombstone (#461).
    .filter((m) => m.thumbPresent && !m.deleted)
    .sort((a, b) => b.mediaTime - a.mediaTime || b.num - a.num);
  const items: MediaItem[] = await Promise.all(
    media.map(async (m) => ({
      mediaId: m.mediaId,
      dir: m.dir,
      num: m.num,
      fileType: m.fileType,
      mediaTime: m.mediaTime,
      videoState: m.videoState,
      thumbUrl: await deps.storage.thumbReadUrl(camera.camId, m.mediaId),
      ...(m.videoState === 'available'
        ? { videoUrl: await deps.storage.videoReadUrl(camera.camId, m.mediaId) }
        : {}),
      ...(m.videoState === 'failed'
        ? { videoError: latest.get(`requestVideo:${m.mediaId}`)?.error }
        : {}),
      ...deleteInfo(latest.get(`deleteMedia:${m.mediaId}`)),
      ...(m.onCamera === false ? { onCamera: false as const } : {}),
    })),
  );
  return json(200, items);
}

function deleteInfo(c: CommandRecord | undefined): Pick<MediaItem, 'deleteState' | 'deleteError'> {
  if (!c) return {};
  if (c.state === 'queued') return { deleteState: 'requested' };
  if (c.state === 'delivered') return { deleteState: 'delivered' };
  if (c.state === 'failed') return { deleteState: 'failed', deleteError: c.error };
  return {};
}

/** At most this many recordings per delete request. */
const MAX_DELETE_PER_REQUEST = 200;

async function createCommand(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const body = parseJsonBody<{
    type?: unknown;
    mediaId?: unknown;
    mediaIds?: unknown;
    targets?: unknown;
  }>(req);
  if (body.type === 'readCamera') return createReadCommand(camera, deps);
  if (body.type === 'setSettings') {
    const result = await requestSettings(camera, (body as { settings?: unknown }).settings, deps);
    if (!result.ok) throw new HttpError(result.status, result.error);
    return json(result.created ? 201 : 200, commandResult(result.command));
  }
  if (body.type === 'deleteMedia') {
    const ids = body.mediaIds;
    if (
      !Array.isArray(ids) ||
      ids.length === 0 ||
      ids.length > MAX_DELETE_PER_REQUEST ||
      !ids.every(isMediaId)
    ) {
      throw new HttpError(400, `mediaIds must be 1-${MAX_DELETE_PER_REQUEST} valid media ids`);
    }
    const hub = await deps.store.getHub(camera.hubId);
    // Without `targets` the hub's setting applies (#461).
    const targets =
      body.targets === undefined ? deleteTargetsOf(hub) : parseDeleteTargets(body.targets);
    if (!targets) {
      throw new HttpError(400, 'targets must be a list of camera, hub or backend, at least one');
    }
    const result = await requestDelete(camera, ids as number[], targets, hub, deps);
    if (!result.ok) throw new HttpError(result.status, result.error);
    return json(result.commands.length > 0 ? 201 : 200, result.commands.map(commandResult));
  }
  if (body.type !== 'requestVideo') throw new HttpError(400, 'Unsupported command type');
  if (!isMediaId(body.mediaId)) throw new HttpError(400, 'mediaId must be a valid media id');
  const result = await requestVideo(camera, body.mediaId, deps);
  if (!result.ok) throw new HttpError(result.status, result.error);
  return json(result.created ? 201 : 200, commandResult(result.command));
}

/**
 * Asks the hub to read the camera at its next check-in. The hub acknowledges
 * the command at the check-in after the read. One open request per camera.
 */
async function createReadCommand(camera: CameraRecord, deps: Deps) {
  const pending = (await deps.store.listCommands(camera.camId)).find(
    (c) => c.type === 'readCamera' && isOpen(c),
  );
  if (pending) return json(200, commandResult(pending));
  const now = deps.now();
  const command: CommandRecord = {
    id: (deps.newId ?? randomUUID)(),
    type: 'readCamera',
    mediaId: 0,
    state: 'queued',
    created: now,
    updated: now,
  };
  await deps.store.putCommand(camera.camId, command);
  return json(201, commandResult(command));
}

/** The hub, if the signed-in user may use it; 404 otherwise. */
async function userHub(req: ApiRequest, hubId: string, deps: Deps) {
  const user = await authUser(req, deps);
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  if (!hub || !(await mayUseHub(user, hubId, deps))) throw new HttpError(404, 'Hub not found');
  return hub;
}

/**
 * Where a delete in the gallery applies for the hub (#461). Any user of the
 * hub may set it; it is the same for all of them.
 */
async function setDeleteTargets(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  const body = parseJsonBody<{ targets?: unknown }>(req);
  const targets = parseDeleteTargets(body.targets);
  if (!targets) {
    throw new HttpError(400, 'targets must be a list of camera, hub or backend, at least one');
  }
  await deps.store.setHubDeleteTargets(hub.hubId, targets);
  return json(200, { targets });
}

/** Cameras the hub found, for the gallery (#297, #298). */
async function listFoundCameras(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  const found = sortForGallery(await deps.store.listFoundCameras(hub.hubId));
  const result = await Promise.all(
    found.map(async (c) => {
      const camera = c.camId ? await deps.store.getCamera(c.camId) : undefined;
      return { ...c, name: camera?.name ?? c.alias ?? c.advName ?? c.bleAddress };
    }),
  );
  return json(200, result);
}

/**
 * Approve a found camera (with a name), exclude it, or approve it again
 * (#297). Every user linked to the hub may decide (decision 16 Nov 2026).
 */
async function decideFoundCamera(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  const bleAddress = normalizeBleAddress(params.address);
  const known = bleAddress && (await deps.store.getFoundCamera(hub.hubId, bleAddress));
  if (!bleAddress || !known) throw new HttpError(404, 'Camera not found');
  const body = parseJsonBody<{ state?: unknown; name?: unknown } | null>(req);
  const state = body?.state;
  if (state !== 'approved' && state !== 'excluded') {
    throw new HttpError(400, 'state must be approved or excluded');
  }
  let alias = known.alias;
  if (body?.name !== undefined) {
    const name = validCameraName(body.name);
    if (name === undefined) {
      throw new HttpError(400, `name must be 1-${MAX_CAMERA_NAME} printable characters`);
    }
    alias = name;
    if (known.camId) await deps.store.renameCamera(known.camId, name);
  }
  const updated = { ...known, state, alias } as const;
  await deps.store.putFoundCamera(hub.hubId, updated);
  return json(200, updated);
}

/**
 * Asks the hub to read its cameras at the next contact (#308): the read's
 * BLE scan notes every camera in range (#296), and the check-in after it
 * reports them. Excluded cameras are left out. A hub without a camera read
 * yet scans at every planned read anyway: `requested` is 0 then.
 */
async function requestScan(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const hub = await userHub(req, params.hubId ?? '', deps);
  const excluded = new Set(
    (await deps.store.listFoundCameras(hub.hubId))
      .filter((c) => c.state === 'excluded' && c.camId)
      .map((c) => c.camId),
  );
  let requested = 0;
  for (const camId of await deps.store.listHubCameraIds(hub.hubId)) {
    const camera = await deps.store.getCamera(camId);
    if (!camera || camera.hubId !== hub.hubId || excluded.has(camId)) continue;
    await createReadCommand(camera, deps);
    requested++;
  }
  return json(200, { requested, nextCheckinAt: hub.schedule?.nextCheckinAt });
}

/**
 * Sets the schedule of a hub (replaces it as a whole). A missing or null
 * field leaves that setting to the hub; the hub takes the change at its next
 * check-in.
 */
async function setSchedule(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  const hubId = params.hubId ?? '';
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  // Same answer for "unknown" and "not yours".
  if (!hub || !(await mayUseHub(user, hubId, deps))) {
    throw new HttpError(404, 'Hub not found');
  }
  const body = parseJsonBody<{
    cameraPlan?: unknown;
    checkinIntervalS?: unknown;
    powerSetting?: unknown;
  }>(req);
  const config: ScheduleConfig = {};
  if (body.cameraPlan !== undefined && body.cameraPlan !== null && body.cameraPlan !== '') {
    if (typeof body.cameraPlan !== 'string' || body.cameraPlan.length > MAX_PLAN_LENGTH) {
      throw new HttpError(400, 'cameraPlan must be a string of at most 200 characters');
    }
    const parsed = parsePlan(body.cameraPlan);
    if (!parsed.ok) throw new HttpError(400, `Invalid cameraPlan: ${parsed.error}`);
    config.cameraPlan = formatPlan(parsed.plan);
  }
  if (body.checkinIntervalS !== undefined && body.checkinIntervalS !== null) {
    const value = body.checkinIntervalS;
    if (
      !Number.isInteger(value) ||
      (value as number) < MIN_CHECKIN_INTERVAL_S ||
      (value as number) > MAX_CHECKIN_INTERVAL_S
    ) {
      throw new HttpError(400, 'checkinIntervalS must be 300-86400 seconds');
    }
    config.checkinIntervalS = value as number;
  }
  if (body.powerSetting !== undefined && body.powerSetting !== null) {
    // The power supply is hardware: only the admin changes it (#431).
    if (!user.admin) throw new HttpError(403, 'Only the admin can set the power supply');
    if (!POWER_SETTINGS.includes(body.powerSetting as PowerSetting)) {
      throw new HttpError(400, 'powerSetting must be auto, mains or battery');
    }
    config.powerSetting = body.powerSetting as PowerSetting;
  } else if (!user.admin && hub.scheduleConfig?.powerSetting) {
    // The schedule is replaced as a whole: a user's change keeps the admin's choice.
    config.powerSetting = hub.scheduleConfig.powerSetting;
  }
  await deps.store.setHubScheduleConfig(hubId, config);
  return json(200, config);
}

/**
 * Withdraws the open requests (video, delete) for a recording that the hub
 * has not picked up yet. Once delivered, the hub may already be at work.
 */
async function shareMedia(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const user = await authUser(req, deps);
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const mediaId = Number(params.mediaId);
  if (!isMediaId(mediaId)) throw new HttpError(400, 'mediaId must be a valid media id');
  return createShare(req, camera, mediaId, user.userId, deps);
}

async function listOwnShares(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  return listShares((await authUser(req, deps)).userId, deps);
}

async function revokeOwnShare(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  return revokeShare((await authUser(req, deps)).userId, params.shareId ?? '', deps);
}

async function withdrawMedia(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const mediaId = Number(params.mediaId);
  if (!isMediaId(mediaId)) throw new HttpError(400, 'mediaId must be a valid media id');
  const result = await withdraw(camera, mediaId, deps);
  if (!result.ok) throw new HttpError(result.status, result.error);
  return json(200, { withdrawn: result.withdrawn });
}

/**
 * Renames a camera. The end user decides what his cameras are called (#273),
 * so every user linked to the camera's hub may do it, not only the admin.
 * POST, because the Function URL's CORS allows only GET and POST.
 */
async function renameCameraByUser(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const body = parseJsonBody<{ name?: unknown } | null>(req);
  const name = validCameraName(body?.name);
  if (name === undefined) {
    throw new HttpError(400, `name must be 1-${MAX_CAMERA_NAME} printable characters`);
  }
  await deps.store.renameCamera(camera.camId, name);
  return json(200, { camId: camera.camId, name });
}

/**
 * Hides a camera in the gallery or shows it again (#273). Reversible: the
 * recordings stay in the backend and on the hub, but the hub no longer
 * reads the camera (it leaves `approvedCameras`). Every user linked to the hub may do it, like renaming.
 */
async function hideCamera(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const hidden = parseJsonBody<{ hidden?: unknown } | null>(req)?.hidden;
  if (typeof hidden !== 'boolean') throw new HttpError(400, 'hidden must be true or false');
  await deps.store.setCameraHidden(camera.camId, hidden);
  return json(200, { camId: camera.camId, hidden });
}

/** Takes back the settings changes the hub has not received yet (#368). */
async function withdrawCameraSettings(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  return json(200, { withdrawn: await withdrawSettings(camera, deps) });
}

function commandResult(c: CommandRecord): CommandResult {
  return { id: c.id, type: c.type, state: c.state };
}

// ---- Hub endpoints -------------------------------------------------------------

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function optionalBoolean(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length <= 64 ? value : undefined;
}

/** A message from the hub, shortened to fit. */
function optionalText(value: unknown): string | undefined {
  return typeof value === 'string' && value.length > 0 ? value.slice(0, 200) : undefined;
}

const SD_CARD_STATES: SdCardState[] = ['ok', 'missing'];

/** The SD card state of a camera report; an error message only with `missing`. */
function sdCardTelemetry(report: CameraReport): Pick<CameraTelemetry, 'sdCard' | 'sdError'> {
  const sdCard = SD_CARD_STATES.find((s) => s === report.sdCard);
  if (sdCard === undefined) return {};
  // An empty message clears the one of an earlier missing card.
  return { sdCard, sdError: sdCard === 'missing' ? (optionalString(report.sdError) ?? '') : '' };
}

const ORIGINS: ScheduleOrigin[] = ['built-in', 'local', 'remote'];

function origin(value: unknown): ScheduleOrigin {
  return ORIGINS.find((o) => o === value) ?? 'built-in';
}

/** A planned time if it is plausible (not in the past, not far ahead). */
function plannedTime(value: unknown, now: number): number | undefined {
  const t = optionalNumber(value);
  return t !== undefined &&
    Number.isInteger(t) &&
    t >= now - MAX_CLOCK_SKEW_S &&
    t <= now + MAX_PLAN_AHEAD_S
    ? t
    : undefined;
}

const STRETCHES: readonly ScheduleStretch[] = [
  'hubBatteryCritical',
  'hubBatteryLow',
  'cameraBatteryLow',
];

/** The hub's schedule report; anything malformed is dropped, not rejected. */
function sanitizeSchedule(raw: unknown, now: number): HubSchedule | undefined {
  if (typeof raw !== 'object' || raw === null) return undefined;
  const r = raw as Record<string, unknown>;
  if (typeof r.cameraPlan !== 'string' || r.cameraPlan.length > MAX_PLAN_LENGTH) return undefined;
  const interval = r.checkinIntervalS;
  return {
    cameraPlan: r.cameraPlan,
    cameraPlanOrigin: origin(r.cameraPlanOrigin),
    checkinIntervalS:
      Number.isInteger(interval) && (interval as number) >= 0 && (interval as number) <= 86_400
        ? (interval as number)
        : 0,
    checkinOrigin: origin(r.checkinOrigin),
    nextCameraReadAt: plannedTime(r.nextCameraReadAt, now),
    nextCheckinAt: plannedTime(r.nextCheckinAt, now),
    sleeps: r.sleeps === true,
    stretch: STRETCHES.includes(r.stretch as ScheduleStretch)
      ? (r.stretch as ScheduleStretch)
      : undefined,
  };
}

function sanitizeMedia(raw: CameraReport['media'], camId: string): MediaRecord[] {
  if (raw === undefined) return [];
  if (!Array.isArray(raw)) throw new HttpError(400, `media of ${camId} must be an array`);
  return raw.map((m, i) => {
    const ok =
      isMediaId(m?.mediaId) &&
      Number.isInteger(m?.dir) &&
      Number.isInteger(m?.num) &&
      (m?.fileType === 0 || m?.fileType === 2) &&
      Number.isInteger(m?.mediaTime);
    if (!ok) throw new HttpError(400, `Invalid media entry ${i} of camera ${camId}`);
    return {
      mediaId: m.mediaId,
      dir: m.dir,
      num: m.num,
      fileType: m.fileType as 0 | 2,
      mediaTime: m.mediaTime,
      thumbPresent: false,
      videoState: 'none',
    };
  });
}

/** Validates the whole request before anything is stored. */
function parseCameraReports(
  body: CheckinRequest,
): Array<CameraReport & { records: MediaRecord[]; removed: number[] }> {
  const raw = body.cameras ?? [];
  if (!Array.isArray(raw)) throw new HttpError(400, 'cameras must be an array');
  if (raw.length > MAX_CAMERAS_PER_CHECKIN) throw new HttpError(413, 'Too many cameras');
  const seen = new Set<string>();
  let mediaCount = 0;
  const reports = raw.map((report, i) => {
    const camId = report?.camId;
    if (typeof camId !== 'string' || !ID_PATTERN.test(camId)) {
      throw new HttpError(400, `Invalid camId at index ${i}`);
    }
    if (seen.has(camId)) throw new HttpError(400, `Camera ${camId} reported twice`);
    seen.add(camId);
    const records = sanitizeMedia(report.media, camId);
    mediaCount += records.length;
    const removed = report.removedMedia ?? [];
    if (
      !Array.isArray(removed) ||
      removed.length > MAX_MEDIA_PER_CHECKIN ||
      !removed.every(isMediaId)
    ) {
      throw new HttpError(400, `Invalid removedMedia for camera ${camId}`);
    }
    return { ...report, camId, records, removed: removed as number[] };
  });
  if (mediaCount > MAX_MEDIA_PER_CHECKIN) throw new HttpError(413, 'Too many media entries');
  return reports;
}

/** At most one sample per hub or camera and hour (#76). */
const SAMPLE_EVERY_S = 60 * 60;

async function recordSample(deps: Deps, owner: SampleOwner, sample: Sample): Promise<void> {
  if (sample.batPercent === undefined && sample.sdFreeMB === undefined) return;
  const recent = await deps.store.listSamples(owner, sample.at - SAMPLE_EVERY_S + 1);
  if (recent.length > 0) return;
  await deps.store.addSample(owner, sample);
}

/** How far back the lowest camera battery value counts (#472). */
const LOW_WINDOW_S = 12 * 60 * 60;

/**
 * The lowest battery value of a camera in the last 12 h including the one just
 * read; a full 100 % counts as charged and stands alone (#472).
 */
export async function lowestRecently(
  deps: Deps,
  camId: string,
  at: number,
  batPercent: number | undefined,
): Promise<number | undefined> {
  if (batPercent === undefined) return undefined;
  if (batPercent >= 100) return batPercent;
  const recent = await deps.store.listSamples({ camId }, at - LOW_WINDOW_S);
  // Values before the last charge say nothing about the level now.
  const charged = recent.filter((s) => s.charging === true || (s.batPercent ?? 0) >= 100);
  const since = Math.max(0, ...charged.map((s) => s.at));
  return Math.min(
    batPercent,
    ...recent.filter((s) => s.at > since && s.at <= at).map((s) => s.batPercent ?? 100),
  );
}

/** The check-in's memory report (#236) with plausible byte counts only. */
export function parseMemory(raw: unknown): HubMemory | undefined {
  if (typeof raw !== 'object' || raw === null) return undefined;
  const r = raw as Record<string, unknown>;
  const bytes = (v: unknown) => {
    const n = optionalNumber(v);
    return n !== undefined && Number.isInteger(n) && n >= 0 && n < 2 ** 31 ? n : undefined;
  };
  const cycle = bytes(r.cycle);
  const heapMin = bytes(r.heapMin);
  if (cycle === undefined || heapMin === undefined) return undefined;
  const memory: HubMemory = { cycle, heapMin };
  for (const key of ['psramMin', 'stackSession', 'stackUplink', 'stackController'] as const) {
    const value = bytes(r[key]);
    if (value !== undefined) memory[key] = value;
  }
  return memory;
}

/** Charging flag and cell voltage of a check-in, only what is plausible (#421). */
export function chargingSample(
  body: Pick<CheckinRequest, 'hubCharging' | 'hubBatMillivolts'>,
): Pick<Sample, 'charging' | 'millivolts'> {
  const sample: Pick<Sample, 'charging' | 'millivolts'> = {};
  if (typeof body.hubCharging === 'boolean') sample.charging = body.hubCharging;
  const mv = optionalNumber(body.hubBatMillivolts);
  if (mv !== undefined && Number.isInteger(mv) && mv >= 2000 && mv <= 5000) sample.millivolts = mv;
  return sample;
}

/** The power mode of a check-in; anything but "mains" or "battery" is ignored (#416). */
export function powerModeOf(body: CheckinRequest): HubPowerMode | undefined {
  return body.hubPowerMode === 'mains' || body.hubPowerMode === 'battery'
    ? body.hubPowerMode
    : undefined;
}

const UPLINKS: readonly HubUplink[] = ['wifi', 'modem', 'ethernet'];

/** Entries of a check-in's `dataUsage` that are looked at; a hub sends one per transport. */
const MAX_USAGE_ENTRIES = 8;

/** More than a hub can send in one check-in (a few GB over a day is a lot). */
const MAX_REPORTED_BYTES = 1_000_000_000_000;

/**
 * The `dataUsage` of a check-in (#415). Malformed entries are skipped; one
 * entry per transport, so a faulty hub cannot inflate the sums by repeating.
 */
function parseDataUsage(raw: unknown): DataUsageReport[] {
  if (!Array.isArray(raw)) return [];
  const out = new Map<HubUplink, DataUsageReport>();
  for (const item of raw.slice(0, MAX_USAGE_ENTRIES) as Array<Record<string, unknown> | null>) {
    const transport = UPLINKS.find((u) => u === item?.transport);
    const tx = item?.txBytes;
    const rx = item?.rxBytes;
    const valid = (n: unknown): n is number =>
      typeof n === 'number' && Number.isInteger(n) && n >= 0 && n <= MAX_REPORTED_BYTES;
    if (!transport || !valid(tx) || !valid(rx) || (tx === 0 && rx === 0)) continue;
    out.set(transport, { transport, txBytes: tx, rxBytes: rx });
  }
  return [...out.values()];
}

// eslint-disable-next-line no-control-regex
const CONTROL_CHARS = /[\u0000-\u001f\u007f]/g;

/**
 * Transport, modem signal and mobile network of a check-in (#149); unknown
 * values are left out. A hub without the operator field (older firmware) sends
 * none.
 */
export function uplinkOf(body: CheckinRequest): {
  uplink?: HubUplink;
  modemSignalDbm?: number;
  modemOperator?: string;
} {
  const uplink = UPLINKS.find((u) => u === body.hubUplink);
  const signal = optionalNumber(body.modemSignalDbm);
  const out: { uplink?: HubUplink; modemSignalDbm?: number; modemOperator?: string } = {};
  if (uplink) out.uplink = uplink;
  if (uplink === 'modem' && signal !== undefined && signal < 0 && signal > -150) {
    out.modemSignalDbm = Math.round(signal);
  }
  const operator =
    typeof body.modemOperator === 'string'
      ? body.modemOperator.replace(CONTROL_CHARS, '').trim().slice(0, 40)
      : '';
  if (uplink === 'modem' && operator) out.modemOperator = operator;
  return out;
}

const UPDATE_RESULTS: readonly FirmwareUpdateResult[] = [
  'installed',
  'rolledBack',
  'failed',
  'skipped',
];

/**
 * The hub's update record (#149) as a firmware change; undefined if absent
 * or malformed. The hub's time if plausible (its clock may not have been set
 * at boot), else the check-in's.
 */
export function parseFirmwareUpdate(raw: unknown, now: number): FirmwareChange | undefined {
  if (typeof raw !== 'object' || raw === null) return undefined;
  const r = raw as Record<string, unknown>;
  const from = optionalString(r.from)?.slice(0, 40);
  const result = UPDATE_RESULTS.find((x) => x === r.result);
  // A `skipped` record may come without `to`: the hub was stopped before it
  // read the manifest and names no release (#363). It is stored with `to: ''`.
  const to = optionalString(r.to)?.slice(0, 40) ?? (result === 'skipped' ? '' : undefined);
  if (!from || to === undefined || !result) return undefined;
  const reason = optionalString(r.reason)?.slice(0, 200);
  const at = optionalNumber(r.at);
  const change: FirmwareChange = {
    at: at !== undefined && at <= now + MAX_CLOCK_SKEW_S && at >= now - MAX_READ_AGE_S ? at : now,
    from,
    to,
    result,
  };
  if (reason) change.reason = reason;
  return change;
}

/** When the hub read the camera: its own timestamp if plausible, else now. */
function readTime(readAt: unknown, now: number): number {
  const t = optionalNumber(readAt);
  return t !== undefined && t <= now + MAX_CLOCK_SKEW_S && t >= now - MAX_READ_AGE_S ? t : now;
}

async function checkin(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const hub = await authHub(req, deps);
  const body = parseJsonBody<CheckinRequest>(req);
  const reports = parseCameraReports(body);
  const now = deps.now();
  const discovered = parseDiscovered(body.discovered, now, readTime);

  // The hub's own record of its last update (#149), and another firmware
  // than last time: an update, a rollback or a USB flash.
  const firmware = optionalString(body.hubFirmware);
  const update = parseFirmwareUpdate(body.firmwareUpdate, now);
  if (update) await deps.store.addFirmwareChange(hub.hubId, update);
  const reportedByHub = update?.result === 'installed' && update.to === firmware;
  if (firmware && hub.firmware && firmware !== hub.firmware && !reportedByHub) {
    await deps.store.addFirmwareChange(hub.hubId, { at: now, from: hub.firmware, to: firmware });
  }
  await deps.store.updateHub(
    hub.hubId,
    {
      firmware,
      batPercent: optionalNumber(body.hubBatPercent),
      powerMode: powerModeOf(body),
      schedule: sanitizeSchedule(body.schedule, now),
      ...uplinkOf(body),
      memory: parseMemory(body.memory),
      features: parseFeatures(body.features),
    },
    now,
    // A hub without a battery reading omits the field: drop the old value, so
    // the admin view shows "unknown" and not a stale percentage.
    [
      ...(optionalNumber(body.hubBatPercent) === undefined ? (['batPercent'] as const) : []),
      // Older firmware sends no mode: do not keep an old one as current.
      ...(powerModeOf(body) === undefined ? (['powerMode'] as const) : []),
      // An older firmware reports none: it cannot do what the last one could.
      ...(parseFeatures(body.features) === undefined ? (['features'] as const) : []),
    ],
  );
  // The data volume since the last check-in, added to the sum of today (#415).
  for (const entry of parseDataUsage(body.dataUsage)) {
    await deps.store.addDataUsage(hub.hubId, utcDay(now), entry);
  }
  await recordSample(
    deps,
    { hubId: hub.hubId },
    {
      at: now,
      batPercent: optionalNumber(body.hubBatPercent),
      ...chargingSample(body),
    },
  );

  // Cameras heard in a scan, read or not (#296).
  if (discovered) await recordDiscovered(deps.store, hub.hubId, discovered);
  let reportsBle = discovered !== undefined;

  const results = new Map<string, CameraCheckin>();
  for (const report of reports) {
    const { camera, created } = await deps.store.claimCamera(report.camId, hub.hubId);
    if (camera.hubId !== hub.hubId) {
      // Cameras stay with the hub that reported them first.
      results.set(report.camId, emptyResult(report.camId, false, false));
      continue;
    }
    const batPercent = optionalNumber(report.batPercent);
    await deps.store.updateCamera(
      camera.camId,
      {
        model: optionalString(report.model),
        firmware: optionalString(report.firmware),
        batPercent,
        batLow12h: await lowestRecently(
          deps,
          camera.camId,
          readTime(report.readAt, now),
          batPercent,
        ),
        sdFreeMB: optionalNumber(report.sdFreeMB),
        sdTotalMB: optionalNumber(report.sdTotalMB),
        ...sdCardTelemetry(report),
        temperature: optionalNumber(report.temperature),
        isCharging: optionalBoolean(report.isCharging),
        isSolarCharging: optionalBoolean(report.isSolarCharging),
        batType: optionalNumber(report.batType),
        bleVersion: optionalString(report.bleVersion),
        cameraName: optionalString(report.cameraName),
        bleAddress: normalizeBleAddress(report.bleAddress),
        // Absent from older firmware: the stored ones stay (#368).
        settings: parseSettingsReport(report.settings),
        capability: parseCapability(report.capability),
      },
      readTime(report.readAt, now),
    );
    // With its BLE address the camera joins the hub's found cameras (#297);
    // a name given in the gallery before the first read becomes its name.
    const bleAddress = normalizeBleAddress(report.bleAddress);
    if (bleAddress) {
      reportsBle = true;
      const alias = await recordRead(
        deps.store,
        hub.hubId,
        camera.camId,
        bleAddress,
        readTime(report.readAt, now),
      );
      if (alias && created) await deps.store.renameCamera(camera.camId, alias);
    }
    // A report repeats the last read; its sample is stored only once.
    await recordSample(
      deps,
      { camId: camera.camId },
      {
        at: readTime(report.readAt, now),
        batPercent: optionalNumber(report.batPercent),
        sdFreeMB: optionalNumber(report.sdFreeMB),
        temperature: optionalNumber(report.temperature),
        // A 100 % while charging says little about the level (#473).
        ...(optionalBoolean(report.isCharging) !== undefined && {
          charging: optionalBoolean(report.isCharging),
        }),
      },
    );
    // Only new recordings are written; existing ones keep their flags.
    await deps.store.addMedia(camera.camId, report.records);
    // No longer on the camera's card (#461).
    if (report.removed.length > 0) await recordRemoved(camera.camId, report.removed, deps);
    // A new camera may have been reported with a partial (delta) list.
    results.set(camera.camId, emptyResult(camera.camId, true, created));
  }

  // A hub that limits its thumbnail uploads (boards without PSRAM, #244) has
  // little memory for the response; each video request carries a presigned
  // URL of about 1.8 KB, so it gets as many as thumbnail uploads (at least
  // one), delivered ones first. Others get every open request.
  let videoBudget = videoRequestLimit(body.maxThumbUploads);

  // Every camera of the hub can have work: missing thumbnails and commands
  // also for cameras the hub did not read in this cycle.
  const work: Array<{ camId: string; mediaId: number; num: number; mediaTime: number }> = [];
  for (const camId of await deps.store.listHubCameraIds(hub.hubId)) {
    const camera = await deps.store.getCamera(camId);
    if (!camera || camera.hubId !== hub.hubId) continue;
    const result = results.get(camId) ?? emptyResult(camId, true, false);

    for (const m of await deps.store.listMedia(camId)) {
      if (!m.thumbPresent && !m.deleted) {
        work.push({ camId, mediaId: m.mediaId, num: m.num, mediaTime: m.mediaTime });
      }
    }

    // Hand out open commands; a delivered command is re-sent until acknowledged.
    // Deletions go out in portions, delivered ones first so they are answered.
    const all = (await deps.store.listCommands(camId)).filter(isOpen);
    const deletes = all
      .filter((c) => c.type === 'deleteMedia')
      .sort((a, b) => Number(b.state === 'delivered') - Number(a.state === 'delivered'))
      .slice(0, MAX_DELETES_PER_CHECKIN);
    const videos = all
      .filter((c) => c.type === 'requestVideo')
      .sort((a, b) => Number(b.state === 'delivered') - Number(a.state === 'delivered'))
      .slice(0, Math.max(0, videoBudget));
    videoBudget -= videos.length;
    const open = all.filter(
      (c) =>
        (c.type !== 'deleteMedia' || deletes.includes(c)) &&
        (c.type !== 'requestVideo' || videos.includes(c)),
    );
    for (const command of open.filter((c) => c.state === 'queued')) {
      await deps.store.putCommand(camId, { ...command, state: 'delivered', updated: now });
      if (command.type === 'requestVideo') {
        await deps.store.setVideoState(camId, command.mediaId, 'downloading');
      }
    }
    result.commands = await Promise.all(
      open.map(async (c) =>
        c.type === 'requestVideo'
          ? {
              id: c.id,
              type: c.type,
              mediaId: c.mediaId,
              uploadUrl: await deps.storage.videoUploadUrl(camId, c.mediaId),
            }
          : c.type === 'deleteMedia'
            ? {
                id: c.id,
                type: c.type,
                mediaId: c.mediaId,
                // The backend's part is its own; a command from before has none.
                ...(c.targets ? { targets: c.targets.filter((t) => t !== 'backend') } : {}),
              }
            : c.type === 'setSettings'
              ? { id: c.id, type: c.type, settings: c.settings ?? {} }
              : { id: c.id, type: c.type },
      ),
    );
    results.set(camId, result);
  }

  // Newest thumbnails first over all cameras, so the latest pictures reach
  // the phone first.
  const maxUploads = thumbUploadLimit(
    body.maxThumbUploads,
    deps.config?.maxThumbUploadsPerCheckin ?? DEFAULT_MAX_THUMB_UPLOADS,
  );
  work.sort((a, b) => b.mediaTime - a.mediaTime || b.num - a.num);
  for (const m of work.slice(0, maxUploads)) {
    results.get(m.camId)?.thumbUploads.push({
      mediaId: m.mediaId,
      url: await deps.storage.thumbUploadUrl(m.camId, m.mediaId),
    });
  }

  const response: CheckinResponse = {
    serverTime: now,
    // Reported cameras plus every other camera that has something to do.
    cameras: [...results.values()].filter(
      (r) =>
        reports.some((report) => report.camId === r.camId) ||
        r.thumbUploads.length > 0 ||
        r.commands.length > 0,
    ),
    // The schedule set in the app; without it the hub keeps its own.
    config: hub.scheduleConfig ?? {},
  };
  // Only for firmware that reports BLE addresses; others read every camera.
  if (reportsBle) response.approvedCameras = await approvedAddresses(deps.store, hub.hubId);
  return json(200, response);
}

/**
 * Thumbnail uploads for one check-in: the backend's limit, lowered by the hub
 * if it asks for fewer (a hub without PSRAM cannot hold a response with 30
 * presigned URLs). Anything but a whole number from 0 up is ignored.
 */
/** Video requests per check-in: unlimited unless the hub limits its uploads. */
export function videoRequestLimit(maxThumbUploads: unknown): number {
  const wanted = optionalNumber(maxThumbUploads);
  if (wanted === undefined || !Number.isInteger(wanted) || wanted < 0) return Infinity;
  return Math.max(1, wanted);
}

function thumbUploadLimit(requested: unknown, backendLimit: number): number {
  const wanted = optionalNumber(requested);
  if (wanted === undefined || !Number.isInteger(wanted) || wanted < 0) return backendLimit;
  return Math.min(wanted, backendLimit);
}

/** The check-in's `features`: short names only, at most 16. */
export function parseFeatures(raw: unknown): string[] | undefined {
  if (!Array.isArray(raw)) return undefined;
  const names = raw.filter(
    (f): f is string => typeof f === 'string' && /^[A-Za-z0-9_-]{1,40}$/.test(f),
  );
  return [...new Set(names)].slice(0, 16);
}

/** From this many recordings at once the loss is logged, so it can be traced. */
const MANY_REMOVED = 20;

/**
 * Recordings the hub reports as no longer on the camera's card (#461). One
 * the gallery asked to delete on the camera, without the gallery, stays here
 * (`onCamera: false`); every other one was lost by the camera itself (deleted
 * there, card formatted) and goes from the gallery and the storage as well.
 * Repeating an id is harmless.
 */
async function recordRemoved(camId: string, mediaIds: number[], deps: Deps): Promise<void> {
  if (mediaIds.length >= MANY_REMOVED) {
    console.warn(JSON.stringify({ manyRemoved: { camId, count: mediaIds.length } }));
  }
  const commands = await deps.store.listCommands(camId);
  for (const mediaId of new Set(mediaIds)) {
    const media = await deps.store.getMedia(camId, mediaId);
    if (!media || media.deleted) continue;
    const asked = commands
      .filter(
        (c) =>
          c.type === 'deleteMedia' &&
          c.mediaId === mediaId &&
          (c.state === 'delivered' || c.state === 'done'),
      )
      .sort((a, b) => b.created - a.created)[0];
    const keep = asked !== undefined && !targetsOf(asked).includes('backend');
    if (keep) {
      if (media.onCamera !== false) await deps.store.markMediaOffCamera(camId, mediaId);
    } else {
      await deleteInBackend(camId, mediaId, deps);
    }
  }
}

function emptyResult(camId: string, accepted: boolean, resync: boolean): CameraCheckin {
  return { camId, accepted, resync, thumbUploads: [], commands: [] };
}

async function ack(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const hub = await authHub(req, deps);
  const body = parseJsonBody<AckRequest>(req);
  const now = deps.now();

  let thumbsConfirmed = 0;
  let commandsUpdated = 0;
  for (const entry of Array.isArray(body.cameras) ? body.cameras : []) {
    if (typeof entry?.camId !== 'string') continue;
    const camera = await deps.store.getCamera(entry.camId);
    // Acknowledgements for cameras of other hubs are ignored.
    if (!camera || camera.hubId !== hub.hubId) continue;

    for (const t of Array.isArray(entry.uploadedThumbs) ? entry.uploadedThumbs : []) {
      if (!isMediaId(t?.mediaId)) continue;
      // Trust S3, not the device: only mark thumbnails that really arrived.
      if (await deps.storage.thumbExists(camera.camId, t.mediaId)) {
        await deps.store.markThumbPresent(camera.camId, t.mediaId);
        thumbsConfirmed++;
      }
    }

    for (const c of Array.isArray(entry.commands) ? entry.commands : []) {
      if (typeof c?.id !== 'string' || (c.state !== 'done' && c.state !== 'failed')) continue;
      const command = await deps.store.getCommand(camera.camId, c.id);
      if (!command || !isOpen(command)) continue;
      let state: 'done' | 'failed' = c.state;
      let error = state === 'failed' ? (optionalText(c.error) ?? 'failed on the hub') : undefined;
      // Trust S3, not the device: a video is available only if it arrived.
      if (
        command.type === 'requestVideo' &&
        state === 'done' &&
        !(await deps.storage.videoExists(camera.camId, command.mediaId))
      ) {
        state = 'failed';
        error = 'the video did not arrive';
      }
      await deps.store.putCommand(camera.camId, { ...command, state, updated: now, error });
      // The camera took the change: shown until its next read confirms it
      // (the hub reports the new values with the check-in, too).
      if (command.type === 'setSettings' && state === 'done' && camera.settings) {
        camera.settings = { ...camera.settings, ...command.settings };
        await deps.store.updateCamera(camera.camId, { settings: camera.settings }, camera.lastSeen);
      }
      if (command.type === 'requestVideo') {
        await deps.store.setVideoState(
          camera.camId,
          command.mediaId,
          state === 'done' ? 'available' : 'failed',
        );
      }
      // The hub did its part: the gallery's follows (#461). A failed delete
      // leaves everything as it was.
      if (
        command.type === 'deleteMedia' &&
        state === 'done' &&
        targetsOf(command).includes('backend')
      ) {
        await deleteInBackend(camera.camId, command.mediaId, deps);
      }
      commandsUpdated++;
    }
  }

  return json(200, { thumbsConfirmed, commandsUpdated });
}
