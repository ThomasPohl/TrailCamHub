import { randomUUID } from 'node:crypto';
import type { Deps } from './api';
import {
  DEFAULT_DELETE_TARGETS,
  DELETE_TARGETS,
  HUB_FEATURE_DELETE_TARGETS,
  type CameraRecord,
  type CameraSettings,
  type CommandRecord,
  type DeleteTarget,
  type HubRecord,
} from './model';
import { checkSettingsChange } from './settings';

export type VideoRequest =
  | { ok: true; created: boolean; command: CommandRecord }
  | { ok: false; status: 404 | 409; error: string };

/**
 * Queues a requestVideo command for the hub, from the app or a bot button.
 * Re-requesting a pending video returns the existing command (idempotent).
 * The caller has checked that the user may see the camera.
 */
export async function requestVideo(
  camera: CameraRecord,
  mediaId: number,
  deps: Deps,
): Promise<VideoRequest> {
  const media = await deps.store.getMedia(camera.camId, mediaId);
  if (!media || media.deleted) return { ok: false, status: 404, error: 'Recording not found' };
  if (media.fileType !== 2) return { ok: false, status: 409, error: 'Recording is not a video' };
  if (media.onCamera === false && media.videoState !== 'available') {
    // Only a copy on the hub could still be sent; the hub does not keep to that.
    return { ok: false, status: 409, error: 'The recording is no longer on the camera' };
  }

  const pending = (await deps.store.listCommands(camera.camId)).find(
    (c) =>
      c.type === 'requestVideo' &&
      c.mediaId === mediaId &&
      (c.state === 'queued' || c.state === 'delivered'),
  );
  if (pending) return { ok: true, created: false, command: pending };

  const now = deps.now();
  const command: CommandRecord = {
    id: (deps.newId ?? randomUUID)(),
    type: 'requestVideo',
    mediaId,
    state: 'queued',
    created: now,
    updated: now,
  };
  await deps.store.putCommand(camera.camId, command);
  await deps.store.setVideoState(camera.camId, mediaId, 'requested');
  return { ok: true, created: true, command };
}

export type DeleteRequest =
  | { ok: true; commands: CommandRecord[]; deletedHere: number[] }
  | { ok: false; status: 404 | 409; error: string };

/**
 * Where a delete applies when the command has no `targets`: commands from
 * before #461 deleted on the camera, and the gallery entry went with it.
 */
export const LEGACY_DELETE_TARGETS: readonly DeleteTarget[] = ['camera', 'backend'];

/** The targets of a deleteMedia command. */
export function targetsOf(command: Pick<CommandRecord, 'targets'>): readonly DeleteTarget[] {
  return command.targets ?? LEGACY_DELETE_TARGETS;
}

/**
 * Where a delete applies for a hub (#461): what was set for it. Without a
 * choice it is DEFAULT_DELETE_TARGETS, but a hub whose firmware does not know
 * `targets` can only delete on the camera, so it keeps doing that.
 */
export function deleteTargetsOf(
  hub: Pick<HubRecord, 'deleteTargets' | 'features'> | undefined,
): DeleteTarget[] {
  if (hub?.deleteTargets?.length) return [...hub.deleteTargets];
  return [...(hubDeletesByTarget(hub) ? DEFAULT_DELETE_TARGETS : LEGACY_DELETE_TARGETS)];
}

/** The hub's firmware knows `targets` (#461). */
export function hubDeletesByTarget(hub: Pick<HubRecord, 'features'> | undefined): boolean {
  return hub?.features?.includes(HUB_FEATURE_DELETE_TARGETS) ?? false;
}

/**
 * A list of targets from a request: at least one, every one valid, none
 * twice. Returns them in the order of DELETE_TARGETS, or undefined.
 */
export function parseDeleteTargets(raw: unknown): DeleteTarget[] | undefined {
  if (!Array.isArray(raw) || raw.length === 0 || raw.length > DELETE_TARGETS.length) {
    return undefined;
  }
  if (!raw.every((t) => DELETE_TARGETS.includes(t as DeleteTarget))) return undefined;
  if (new Set(raw).size !== raw.length) return undefined;
  return DELETE_TARGETS.filter((t) => raw.includes(t));
}

/**
 * Deletes a recording in the gallery (#461): the record stays as a tombstone
 * (a hub that reports it again must not bring it back), the thumbnail and
 * the video are removed from the storage. A storage error is logged and the
 * recording is gone for the user anyway; the bucket's lifecycle rules remove
 * what is left.
 */
export async function deleteInBackend(camId: string, mediaId: number, deps: Deps): Promise<void> {
  await deps.store.markMediaDeleted(camId, mediaId);
  try {
    await deps.storage.deleteMedia(camId, mediaId);
  } catch (error) {
    console.error(JSON.stringify({ deleteMediaFailed: { camId, mediaId, error: String(error) } }));
  }
}

/**
 * Deletes recordings where `targets` says (#260, #461). Recordings with an
 * open delete keep it (idempotent). Unknown ids fail the whole request, so
 * nothing is half done. The caller has checked that the user may see the
 * camera.
 *
 * - `backend` alone needs no hub: the recordings are gone from the gallery at
 *   once (`deletedHere`).
 * - With `camera` or `hub` the hub gets one command per recording; the part in
 *   the backend follows when the hub reports it done.
 * - A hub whose firmware does not know `targets` always deletes on the
 *   camera: it gets only a delete for the camera, and nothing else (409).
 */
export async function requestDelete(
  camera: CameraRecord,
  mediaIds: number[],
  targets: readonly DeleteTarget[],
  hub: HubRecord | undefined,
  deps: Deps,
): Promise<DeleteRequest> {
  const known = new Set(
    (await deps.store.listMedia(camera.camId)).filter((m) => !m.deleted).map((m) => m.mediaId),
  );
  const unknown = mediaIds.find((id) => !known.has(id));
  if (unknown !== undefined) {
    return { ok: false, status: 404, error: `Recording ${unknown} not found` };
  }
  const onHub = targets.filter((t) => t !== 'backend');
  if (
    onHub.length > 0 &&
    !hubDeletesByTarget(hub) &&
    !(onHub.length === 1 && onHub[0] === 'camera')
  ) {
    return {
      ok: false,
      status: 409,
      error: 'The hub needs a newer firmware to delete on the hub or to keep the camera',
    };
  }
  const ids = [...new Set(mediaIds)];
  if (onHub.length === 0) {
    for (const mediaId of ids) await deleteInBackend(camera.camId, mediaId, deps);
    return { ok: true, commands: [], deletedHere: ids };
  }

  const existing = await deps.store.listCommands(camera.camId);
  const now = deps.now();
  const commands: CommandRecord[] = [];
  for (const mediaId of ids) {
    const pending = existing.find(
      (c) =>
        c.type === 'deleteMedia' &&
        c.mediaId === mediaId &&
        (c.state === 'queued' || c.state === 'delivered'),
    );
    if (pending) {
      commands.push(pending);
      continue;
    }
    const command: CommandRecord = {
      id: (deps.newId ?? randomUUID)(),
      type: 'deleteMedia',
      mediaId,
      targets: [...targets],
      state: 'queued',
      created: now,
      updated: now,
    };
    await deps.store.putCommand(camera.camId, command);
    commands.push(command);
  }
  return { ok: true, commands, deletedHere: [] };
}

export type SettingsRequest =
  | { ok: true; created: boolean; command: CommandRecord }
  | { ok: false; status: 400; error: string };

/**
 * Queues a setSettings command (#368). A change that still waits for the hub
 * takes the new fields (the latest value wins), so the hub gets one 513; one
 * the hub has received stays as it is and the new one follows it. The caller
 * has checked that the user may see the camera.
 */
export async function requestSettings(
  camera: CameraRecord,
  change: unknown,
  deps: Deps,
): Promise<SettingsRequest> {
  const checked = checkSettingsChange(change, camera.settings, camera.capability);
  if (!checked.ok) return { ok: false, status: 400, error: checked.error };

  const now = deps.now();
  const queued = (await deps.store.listCommands(camera.camId)).find(
    (c) => c.type === 'setSettings' && c.state === 'queued',
  );
  if (queued) {
    const settings: CameraSettings = { ...queued.settings, ...checked.settings };
    const command: CommandRecord = { ...queued, settings, updated: now };
    await deps.store.putCommand(camera.camId, command);
    return { ok: true, created: false, command };
  }
  const command: CommandRecord = {
    id: (deps.newId ?? randomUUID)(),
    type: 'setSettings',
    mediaId: 0,
    settings: checked.settings,
    state: 'queued',
    created: now,
    updated: now,
  };
  await deps.store.putCommand(camera.camId, command);
  return { ok: true, created: true, command };
}

/**
 * Takes back the settings changes the hub has not received yet (#368).
 * Returns how many; one the hub has received cannot be taken back.
 */
export async function withdrawSettings(camera: CameraRecord, deps: Deps): Promise<number> {
  const now = deps.now();
  let withdrawn = 0;
  for (const c of await deps.store.listCommands(camera.camId)) {
    if (c.type !== 'setSettings' || c.state !== 'queued') continue;
    await deps.store.putCommand(camera.camId, { ...c, state: 'cancelled', updated: now });
    withdrawn++;
  }
  return withdrawn;
}

export type Withdrawal =
  | { ok: true; withdrawn: number }
  | { ok: false; status: 409; error: string };

/**
 * Cancels the queued video and delete requests of a recording. A request the
 * hub has already received cannot be taken back (409).
 */
export async function withdraw(
  camera: CameraRecord,
  mediaId: number,
  deps: Deps,
): Promise<Withdrawal> {
  const mine = (await deps.store.listCommands(camera.camId)).filter(
    (c) => c.mediaId === mediaId && (c.type === 'requestVideo' || c.type === 'deleteMedia'),
  );
  if (mine.some((c) => c.state === 'delivered')) {
    return { ok: false, status: 409, error: 'The hub has already received the request' };
  }
  const now = deps.now();
  let withdrawn = 0;
  for (const c of mine.filter((c) => c.state === 'queued')) {
    await deps.store.putCommand(camera.camId, { ...c, state: 'cancelled', updated: now });
    if (c.type === 'requestVideo') await deps.store.setVideoState(camera.camId, mediaId, 'none');
    withdrawn++;
  }
  return { ok: true, withdrawn };
}
