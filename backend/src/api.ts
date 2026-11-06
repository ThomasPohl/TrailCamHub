import { randomUUID, timingSafeEqual } from 'node:crypto';
import { parseDeviceKey, verifySecret } from './auth/device';
import { validateInitData } from './auth/telegram';
import { requestVideo } from './commands';
import { firmwareOffer, type FirmwareManifest } from './firmware';
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
  Camera,
  CameraCheckin,
  CameraRecord,
  CameraReport,
  CheckinRequest,
  CheckinResponse,
  CommandRecord,
  CommandResult,
  HubRecord,
  HubSchedule,
  MediaItem,
  MediaRecord,
  ScheduleConfig,
  ScheduleOrigin,
  SdCardState,
} from './model';
import { ID_PATTERN, isMediaId } from './model';
import { createPairingCode, HUB_CODE_TTL_S, pairingLink } from './pairing';
import {
  formatPlan,
  MAX_CHECKIN_INTERVAL_S,
  MAX_PLAN_LENGTH,
  MIN_CHECKIN_INTERVAL_S,
  parsePlan,
} from './schedule';
import type { MediaStorage } from './storage';
import type { CameraTelemetry, Store } from './store/types';
import { webhookSecret, type TelegramApi } from './telegram/api';
import { handleUpdate, type TelegramUpdate } from './telegram/bot';
import { notifyNewThumbnails, type NewThumbnail } from './telegram/notify';

export interface Deps {
  store: Store;
  storage: MediaStorage;
  /** Resolved lazily so requests that don't need it never touch SSM. */
  botToken: () => Promise<string>;
  now: () => number;
  newId?: () => string;
  /** The latest firmware release (docs/ota.md); without it no updates are offered. */
  firmwareManifest?: () => Promise<FirmwareManifest | undefined>;
  config?: {
    /** Thumbnail upload URLs handed out per check-in, over all cameras. */
    maxThumbUploadsPerCheckin?: number;
  };
  /** The bot: webhook, notifications, pairing links. Without it these are off. */
  telegram?: {
    api: TelegramApi;
    /** URL of the Mini App for "Open app" buttons; no button without it. */
    appUrl?: string;
    /** Time zone of a user's quiet hours until they set their own. */
    defaultTimeZone: string;
  };
}

const DEFAULT_MAX_THUMB_UPLOADS = 30;
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
  { method: 'GET', pattern: '/v1/cameras', handler: listCameras },
  { method: 'GET', pattern: '/v1/cameras/:camId/media', handler: listMedia },
  { method: 'POST', pattern: '/v1/cameras/:camId/commands', handler: createCommand },
  { method: 'PUT', pattern: '/v1/hubs/:hubId/schedule', handler: setSchedule },
  { method: 'POST', pattern: '/v1/device/checkin', handler: checkin },
  { method: 'POST', pattern: '/v1/device/ack', handler: ack },
  { method: 'POST', pattern: '/v1/device/pairing-code', handler: devicePairingCode },
  { method: 'POST', pattern: '/v1/telegram/webhook', handler: telegramWebhook },
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

async function authUser(req: ApiRequest, deps: Deps): Promise<number> {
  const result = validateInitData(bearer(req, 'tma'), await deps.botToken(), deps.now());
  if (!result.ok) throw new HttpError(401, `Invalid Telegram init data (${result.reason})`);
  return result.identity.userId;
}

async function authUserForCamera(
  req: ApiRequest,
  camId: string,
  deps: Deps,
): Promise<CameraRecord> {
  const userId = await authUser(req, deps);
  const camera = await deps.store.getCamera(camId);
  // Same answer for "unknown" and "not yours", so camera ids cannot be probed.
  if (!camera || !(await deps.store.isLinked(userId, camera.hubId))) {
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
    hubId: record.hubId,
    hubName: hub?.name ?? record.hubId,
    model: record.model ?? 'unknown',
    firmware: record.firmware ?? 'unknown',
    batPercent: record.batPercent ?? 0,
    sdFreeMB: record.sdFreeMB ?? 0,
    sdTotalMB: record.sdTotalMB,
    sdCard: record.sdCard ?? 'unknown',
    sdError: record.sdCard === 'missing' && record.sdError ? record.sdError : undefined,
    hubBatPercent: hub?.batPercent,
    lastSeen: record.lastSeen,
    hubLastSeen: hub?.lastSeen ?? 0,
    hubSchedule: hub?.schedule,
    hubScheduleConfig: hub?.scheduleConfig ?? {},
    readRequested: commands.some((c) => c.type === 'readCamera' && isOpen(c)),
  };
}

async function listCameras(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const userId = await authUser(req, deps);
  const cameras: Camera[] = [];
  for (const hubId of await deps.store.listUserHubIds(userId)) {
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
  const media = (await deps.store.listMedia(camera.camId))
    .filter((m) => m.thumbPresent)
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
    })),
  );
  return json(200, items);
}

async function createCommand(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const body = parseJsonBody<{ type?: unknown; mediaId?: unknown }>(req);
  if (body.type === 'readCamera') return createReadCommand(camera, deps);
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

/**
 * Sets the schedule of a hub (replaces it as a whole). A missing or null
 * field leaves that setting to the hub; the hub takes the change at its next
 * check-in.
 */
async function setSchedule(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const userId = await authUser(req, deps);
  const hubId = params.hubId ?? '';
  const hub = ID_PATTERN.test(hubId) ? await deps.store.getHub(hubId) : undefined;
  // Same answer for "unknown" and "not yours".
  if (!hub || !(await deps.store.isLinked(userId, hubId))) {
    throw new HttpError(404, 'Hub not found');
  }
  const body = parseJsonBody<{ cameraPlan?: unknown; checkinIntervalS?: unknown }>(req);
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
  await deps.store.setHubScheduleConfig(hubId, config);
  return json(200, config);
}

function commandResult(c: CommandRecord): CommandResult {
  return { id: c.id, type: c.type, state: c.state };
}

// ---- Hub endpoints -------------------------------------------------------------

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
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
): Array<CameraReport & { records: MediaRecord[] }> {
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
    return { ...report, camId, records };
  });
  if (mediaCount > MAX_MEDIA_PER_CHECKIN) throw new HttpError(413, 'Too many media entries');
  return reports;
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

  await deps.store.updateHub(
    hub.hubId,
    {
      firmware: optionalString(body.hubFirmware),
      batPercent: optionalNumber(body.hubBatPercent),
      schedule: sanitizeSchedule(body.schedule, now),
    },
    now,
  );

  const results = new Map<string, CameraCheckin>();
  for (const report of reports) {
    const { camera, created } = await deps.store.claimCamera(report.camId, hub.hubId);
    if (camera.hubId !== hub.hubId) {
      // Cameras stay with the hub that reported them first.
      results.set(report.camId, emptyResult(report.camId, false, false));
      continue;
    }
    await deps.store.updateCamera(
      camera.camId,
      {
        model: optionalString(report.model),
        firmware: optionalString(report.firmware),
        batPercent: optionalNumber(report.batPercent),
        sdFreeMB: optionalNumber(report.sdFreeMB),
        sdTotalMB: optionalNumber(report.sdTotalMB),
        ...sdCardTelemetry(report),
      },
      readTime(report.readAt, now),
    );
    // Only new recordings are written; existing ones keep their flags.
    await deps.store.addMedia(camera.camId, report.records);
    // A new camera may have been reported with a partial (delta) list.
    results.set(camera.camId, emptyResult(camera.camId, true, created));
  }

  // Every camera of the hub can have work: missing thumbnails and commands
  // also for cameras the hub did not read in this cycle.
  const work: Array<{ camId: string; mediaId: number; num: number; mediaTime: number }> = [];
  for (const camId of await deps.store.listHubCameraIds(hub.hubId)) {
    const camera = await deps.store.getCamera(camId);
    if (!camera || camera.hubId !== hub.hubId) continue;
    const result = results.get(camId) ?? emptyResult(camId, true, false);

    for (const m of await deps.store.listMedia(camId)) {
      if (!m.thumbPresent) {
        work.push({ camId, mediaId: m.mediaId, num: m.num, mediaTime: m.mediaTime });
      }
    }

    // Hand out open commands; a delivered command is re-sent until acknowledged.
    const open = (await deps.store.listCommands(camId)).filter(isOpen);
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
          : { id: c.id, type: c.type },
      ),
    );
    results.set(camId, result);
  }

  // Newest thumbnails first over all cameras, so the latest pictures reach
  // the phone first.
  const maxUploads = deps.config?.maxThumbUploadsPerCheckin ?? DEFAULT_MAX_THUMB_UPLOADS;
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
  const offer = firmwareOffer(await deps.firmwareManifest?.(), {
    firmware: body.hubFirmware,
    board: body.hubBoard,
    variant: body.hubVariant,
  });
  if (offer) response.firmware = offer;
  return json(200, response);
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
  const newThumbnails: NewThumbnail[] = [];
  for (const entry of Array.isArray(body.cameras) ? body.cameras : []) {
    if (typeof entry?.camId !== 'string') continue;
    const camera = await deps.store.getCamera(entry.camId);
    // Acknowledgements for cameras of other hubs are ignored.
    if (!camera || camera.hubId !== hub.hubId) continue;

    for (const t of Array.isArray(entry.uploadedThumbs) ? entry.uploadedThumbs : []) {
      if (!isMediaId(t?.mediaId)) continue;
      // Trust S3, not the device: only mark thumbnails that really arrived.
      if (await deps.storage.thumbExists(camera.camId, t.mediaId)) {
        const media = await deps.store.getMedia(camera.camId, t.mediaId);
        await deps.store.markThumbPresent(camera.camId, t.mediaId);
        thumbsConfirmed++;
        // A repeated acknowledgement does not announce the recording again.
        if (media && !media.thumbPresent) newThumbnails.push({ camera, media });
      }
    }

    for (const c of Array.isArray(entry.commands) ? entry.commands : []) {
      if (typeof c?.id !== 'string' || (c.state !== 'done' && c.state !== 'failed')) continue;
      const command = await deps.store.getCommand(camera.camId, c.id);
      if (!command || command.state === 'done' || command.state === 'failed') continue;
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
      if (command.type === 'requestVideo') {
        await deps.store.setVideoState(
          camera.camId,
          command.mediaId,
          state === 'done' ? 'available' : 'failed',
        );
      }
      commandsUpdated++;
    }
  }

  try {
    await notifyNewThumbnails(hub, newThumbnails, deps);
  } catch (error) {
    console.error('Notifications failed', error);
  }
  return json(200, { thumbsConfirmed, commandsUpdated });
}

/**
 * A one-time pairing code for the hub to show (display or web UI, as a QR
 * code of `link`). Whoever opens the link gets access to the hub's cameras,
 * so the code is short-lived.
 */
async function devicePairingCode(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const hub = await authHub(req, deps);
  if (!deps.telegram) throw new HttpError(503, 'The bot is not configured');
  const pairing = await createPairingCode(deps.store, hub.hubId, HUB_CODE_TTL_S, deps.now());
  return json(201, {
    code: pairing.code,
    link: pairingLink(await deps.telegram.api.botUsername(), pairing.code),
    expiresAt: pairing.expiresAt,
  });
}

function sameSecret(a: string, b: string): boolean {
  const left = Buffer.from(a);
  const right = Buffer.from(b);
  return left.length === right.length && timingSafeEqual(left, right);
}

/**
 * Updates from Telegram (set up with setWebhook on deploy). Telegram proves
 * itself with the secret token header. Errors are logged and still answered
 * with 200: Telegram would otherwise retry the same update again and again.
 */
async function telegramWebhook(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const secret = req.headers['x-telegram-bot-api-secret-token'] ?? '';
  if (!sameSecret(secret, webhookSecret(await deps.botToken()))) {
    throw new HttpError(401, 'Invalid webhook secret');
  }
  const update = parseJsonBody<TelegramUpdate>(req);
  try {
    await handleUpdate(update, deps);
  } catch (error) {
    console.error(`Telegram update ${String(update?.update_id)} failed`, error);
  }
  return json(200, { ok: true });
}
