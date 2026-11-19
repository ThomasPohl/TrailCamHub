import { createHmac, timingSafeEqual } from 'node:crypto';
import type { AppUser } from '../model';

/**
 * Admin session (#259): what an admin gets for a valid TOTP code. It has the
 * form `<userId>.<expires>.<signature>` and travels as `Authorization: Admin
 * <session>`. The signature key is the user's token hash plus the TOTP secret,
 * so rotating or revoking either one ends every session at once, and no
 * server-side secret or session table is needed.
 */
export const ADMIN_SESSION_S = 15 * 60;

function sign(user: AppUser, userId: number, expires: number): string | undefined {
  if (!user.tokenHash || !user.totpSecret) return undefined;
  return createHmac('sha256', `${user.tokenHash}:${user.totpSecret}`)
    .update(`${userId}.${expires}`)
    .digest('base64url');
}

export function createAdminSession(user: AppUser, nowS: number): string {
  const expires = nowS + ADMIN_SESSION_S;
  const signature = sign(user, user.userId, expires);
  if (!signature) throw new Error('user cannot have an admin session');
  return `${user.userId}.${expires}.${signature}`;
}

export function parseAdminSession(
  session: string,
): { userId: number; expires: number; signature: string } | undefined {
  const match = /^(\d{16})\.(\d{9,12})\.([A-Za-z0-9_-]{43})$/.exec(session);
  if (!match) return undefined;
  return { userId: Number(match[1]), expires: Number(match[2]), signature: match[3] as string };
}

export function verifyAdminSession(
  user: AppUser,
  parsed: { userId: number; expires: number; signature: string },
  nowS: number,
): boolean {
  if (user.role !== 'admin' || parsed.expires <= nowS) return false;
  const expected = sign(user, parsed.userId, parsed.expires);
  if (!expected) return false;
  const a = Buffer.from(expected);
  const b = Buffer.from(parsed.signature);
  return a.length === b.length && timingSafeEqual(a, b);
}
