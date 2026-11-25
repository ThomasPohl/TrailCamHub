import { createHash, randomBytes } from 'node:crypto';
import type { Deps } from './api';
import { HttpError, json, parseJsonBody, type ApiRequest, type ApiResponse } from './http';
import type { CameraRecord, ShareRecord } from './model';

/**
 * Links to a single recording that open without an app token (#444).
 *
 * The link carries a random 128-bit token; only its SHA-256 is stored, like
 * the app tokens. The page behind it answers with a fresh presigned S3 URL on
 * every visit: a presigned URL itself cannot be the link, it stops working
 * when the function's temporary credentials expire.
 */

export const SHARE_DAYS: readonly number[] = [1, 7, 30];
/** Active links per user. */
export const MAX_SHARES_PER_USER = 50;
const TOKEN = /^[A-Za-z0-9_-]{22}$/;

const hashToken = (token: string) => createHash('sha256').update(token).digest('hex');

/** What the owner sees of a link; the token is only in the answer that creates it. */
export interface ShareView {
  id: string;
  camId: string;
  mediaId: number;
  kind: 'thumb' | 'video';
  showCamera: boolean;
  created: number;
  expiresAt: number;
}

function view(share: ShareRecord): ShareView {
  return {
    id: share.shareId,
    camId: share.camId,
    mediaId: share.mediaId,
    kind: share.kind,
    showCamera: share.showCamera,
    created: share.created,
    expiresAt: share.expiresAt,
  };
}

/** `POST /v1/cameras/:camId/media/:mediaId/share`; the caller checked the camera. */
export async function createShare(
  req: ApiRequest,
  camera: CameraRecord,
  mediaId: number,
  userId: number,
  deps: Deps,
): Promise<ApiResponse> {
  const body = parseJsonBody<{ days?: unknown; kind?: unknown; showCamera?: unknown } | null>(req);
  const days = body?.days;
  const kind = body?.kind;
  if (typeof days !== 'number' || !SHARE_DAYS.includes(days)) {
    throw new HttpError(400, `days must be one of ${SHARE_DAYS.join(', ')}`);
  }
  if (kind !== 'thumb' && kind !== 'video') throw new HttpError(400, 'kind must be thumb or video');
  const showCamera = body?.showCamera === true;

  const media = await deps.store.getMedia(camera.camId, mediaId);
  if (!media || media.deleted) throw new HttpError(404, 'Recording not found');
  if (kind === 'thumb' && !media.thumbPresent) throw new HttpError(409, 'No picture yet');
  if (kind === 'video' && media.videoState !== 'available') {
    throw new HttpError(409, 'The video is not in the cloud yet');
  }

  const now = deps.now();
  const active = (await deps.store.listUserShares(userId)).filter((s) => s.expiresAt > now);
  if (active.length >= MAX_SHARES_PER_USER) {
    throw new HttpError(429, `At most ${MAX_SHARES_PER_USER} active links; revoke one first`);
  }

  const token = randomBytes(16).toString('base64url');
  const share: ShareRecord = {
    shareId: randomBytes(4).toString('hex'),
    tokenHash: hashToken(token),
    camId: camera.camId,
    mediaId,
    kind,
    showCamera,
    createdBy: userId,
    created: now,
    expiresAt: now + days * 24 * 60 * 60,
  };
  await deps.store.putShare(share);
  await deps.store.addAudit({ at: now, actor: userId, action: 'shareCreated' });

  const host = req.headers.host;
  const url = `${host ? `https://${host}` : ''}/v1/s/${token}`;
  return json(200, { ...view(share), url });
}

/** `GET /v1/shares`: the user's own active links. */
export async function listShares(userId: number, deps: Deps): Promise<ApiResponse> {
  const now = deps.now();
  const shares = (await deps.store.listUserShares(userId)).filter((s) => s.expiresAt > now);
  return json(200, shares.map(view));
}

/** `POST /v1/shares/:id/revoke`. */
export async function revokeShare(
  userId: number,
  shareId: string,
  deps: Deps,
): Promise<ApiResponse> {
  const share = await deps.store.getUserShare(userId, shareId);
  if (!share) throw new HttpError(404, 'Link not found');
  await deps.store.deleteShare(share);
  await deps.store.addAudit({ at: deps.now(), actor: userId, action: 'shareRevoked' });
  return json(200, { id: shareId });
}

// ---- The public page -----------------------------------------------------------

const escape = (text: string) => text.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

/** "24.11.2026 18:30": the camera's wall clock, read as if it were UTC (docs/clock.md). */
function cameraTime(mediaTime: number): string {
  const d = new Date(mediaTime * 1000);
  const p = (n: number) => String(n).padStart(2, '0');
  return `${p(d.getUTCDate())}.${p(d.getUTCMonth() + 1)}.${d.getUTCFullYear()} ${p(d.getUTCHours())}:${p(d.getUTCMinutes())}`;
}

const PAGE_HEADERS = {
  'content-type': 'text/html; charset=utf-8',
  'cache-control': 'private, no-store',
  'referrer-policy': 'no-referrer',
  'x-robots-tag': 'noindex, nofollow',
  'x-content-type-options': 'nosniff',
  // Only the recording itself may load, from the bucket.
  'content-security-policy':
    "default-src 'none'; img-src https://*.amazonaws.com; media-src https://*.amazonaws.com; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
};

function page(status: number, title: string, body: string): ApiResponse {
  return {
    statusCode: status,
    headers: PAGE_HEADERS,
    body:
      `<!doctype html><html lang="de"><head><meta charset="utf-8">` +
      `<meta name="viewport" content="width=device-width,initial-scale=1">` +
      `<meta name="robots" content="noindex"><title>${escape(title)}</title>` +
      `<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee;` +
      `display:grid;place-items:center;min-height:100vh;padding:1rem;box-sizing:border-box}` +
      `main{max-width:56rem;width:100%}img,video{width:100%;height:auto;border-radius:.5rem;background:#000}` +
      `p{opacity:.8}</style></head><body><main>${body}</main></body></html>`,
  };
}

/** `GET /v1/s/:token`, without sign-in. Unknown and malformed tokens look the same. */
export async function showShare(token: string, deps: Deps): Promise<ApiResponse> {
  const gone = () =>
    page(
      410,
      'Nicht mehr verfügbar',
      '<h1>Nicht mehr verfügbar</h1><p>Dieser Link ist abgelaufen oder wurde zurückgezogen.</p>',
    );
  if (!TOKEN.test(token)) return gone();
  const share = await deps.store.getShareByTokenHash(hashToken(token));
  if (!share || share.expiresAt <= deps.now()) return gone();

  const media = await deps.store.getMedia(share.camId, share.mediaId);
  const ready =
    media && (share.kind === 'thumb' ? media.thumbPresent : media.videoState === 'available');
  if (!media || media.deleted || !ready) return gone();

  const url =
    share.kind === 'thumb'
      ? await deps.storage.thumbReadUrl(share.camId, share.mediaId)
      : await deps.storage.videoReadUrl(share.camId, share.mediaId);
  const camera = share.showCamera ? await deps.store.getCamera(share.camId) : undefined;
  const caption = [camera?.name, cameraTime(media.mediaTime)].filter(Boolean).join(' · ');
  const element =
    share.kind === 'thumb'
      ? `<img src="${escape(url)}" alt="Aufnahme vom ${escape(cameraTime(media.mediaTime))}">`
      : `<video src="${escape(url)}" controls playsinline preload="metadata"></video>`;
  return page(200, 'Aufnahme', `${element}<p>${escape(caption)}</p>`);
}
