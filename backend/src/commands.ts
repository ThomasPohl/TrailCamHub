import { randomUUID } from 'node:crypto';
import type { Deps } from './api';
import type { CameraRecord, CommandRecord } from './model';

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
  if (!media) return { ok: false, status: 404, error: 'Recording not found' };
  if (media.fileType !== 2) return { ok: false, status: 409, error: 'Recording is not a video' };

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
  | { ok: true; commands: CommandRecord[] }
  | { ok: false; status: 404; error: string };

/**
 * Queues one deleteMedia command per recording (#260). Recordings with an
 * open delete keep it (idempotent). Unknown ids fail the whole request, so
 * nothing is half done. The caller has checked that the user may see the
 * camera.
 */
export async function requestDelete(
  camera: CameraRecord,
  mediaIds: number[],
  deps: Deps,
): Promise<DeleteRequest> {
  const known = new Set((await deps.store.listMedia(camera.camId)).map((m) => m.mediaId));
  const unknown = mediaIds.find((id) => !known.has(id));
  if (unknown !== undefined) {
    return { ok: false, status: 404, error: `Recording ${unknown} not found` };
  }
  const existing = await deps.store.listCommands(camera.camId);
  const now = deps.now();
  const commands: CommandRecord[] = [];
  for (const mediaId of new Set(mediaIds)) {
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
      state: 'queued',
      created: now,
      updated: now,
    };
    await deps.store.putCommand(camera.camId, command);
    commands.push(command);
  }
  return { ok: true, commands };
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
