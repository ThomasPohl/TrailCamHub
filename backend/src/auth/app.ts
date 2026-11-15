import { randomBytes, randomInt } from 'node:crypto';
import { hashSecret } from './device';

/**
 * App tokens of gallery users (#259) have the form `<userId>.<secret>`, like
 * hub keys. User ids start at 10^15, far above Telegram user ids, so both
 * kinds of users can be linked to hubs the same way.
 */
export const APP_USER_ID_MIN = 1_000_000_000_000_000;
const APP_USER_ID_MAX = 9_000_000_000_000_000;

export function isAppUserId(id: number): boolean {
  return Number.isSafeInteger(id) && id >= APP_USER_ID_MIN && id < APP_USER_ID_MAX;
}

export function parseAppToken(token: string): { userId: number; secret: string } | undefined {
  const match = /^(\d{16})\.([A-Za-z0-9_-]{16,128})$/.exec(token);
  if (!match) return undefined;
  const userId = Number(match[1]);
  return isAppUserId(userId) ? { userId, secret: match[2] as string } : undefined;
}

export function newAppUserId(): number {
  // randomInt takes at most 2^48 as its range, so the id is built in two parts.
  const high = randomInt(APP_USER_ID_MIN / 1_000_000, APP_USER_ID_MAX / 1_000_000);
  return high * 1_000_000 + randomInt(0, 1_000_000);
}

export function generateAppToken(userId: number): { token: string; tokenHash: string } {
  const secret = randomBytes(32).toString('base64url');
  return { token: `${userId}.${secret}`, tokenHash: hashSecret(secret) };
}
