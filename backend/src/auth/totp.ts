import { createHmac, randomBytes } from 'node:crypto';

/**
 * TOTP (RFC 6238: HMAC-SHA1, 6 digits, 30 s) for the admin's second factor
 * (#259). Written against `node:crypto` so the backend needs no dependency.
 */
export const TOTP_STEP_S = 30;
const DIGITS = 6;
const BASE32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';

export function base32Encode(bytes: Uint8Array): string {
  let bits = 0;
  let value = 0;
  let out = '';
  for (const byte of bytes) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out += BASE32[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += BASE32[(value << (5 - bits)) & 31];
  return out;
}

export function base32Decode(text: string): Buffer | undefined {
  const clean = text.replace(/=+$/, '').toUpperCase();
  if (!/^[A-Z2-7]+$/.test(clean)) return undefined;
  let bits = 0;
  let value = 0;
  const out: number[] = [];
  for (const char of clean) {
    value = (value << 5) | BASE32.indexOf(char);
    bits += 5;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 255);
      bits -= 8;
    }
  }
  return Buffer.from(out);
}

/** A new secret: 20 random bytes (the size of the SHA-1 block output), base32. */
export function generateTotpSecret(): string {
  return base32Encode(randomBytes(20));
}

/** The code for time step `step` (`floor(unixSeconds / 30)`). */
export function totpCode(secret: string, step: number): string {
  const key = base32Decode(secret);
  if (!key || key.length === 0) throw new Error('invalid TOTP secret');
  const counter = Buffer.alloc(8);
  counter.writeBigUInt64BE(BigInt(step));
  const mac = createHmac('sha1', key).update(counter).digest();
  const offset = (mac[mac.length - 1] as number) & 15;
  const binary = mac.readUInt32BE(offset) & 0x7fffffff;
  return String(binary % 10 ** DIGITS).padStart(DIGITS, '0');
}

/**
 * Checks `code` for the current step and one step either side (clock skew).
 * Returns the matching step, or undefined. A step at or below `lastStep` is
 * refused, so a code works only once (replay protection).
 */
export function verifyTotp(
  secret: string,
  code: string,
  nowS: number,
  lastStep = 0,
): number | undefined {
  if (!/^\d{6}$/.test(code)) return undefined;
  const current = Math.floor(nowS / TOTP_STEP_S);
  for (const step of [current - 1, current, current + 1]) {
    if (step > lastStep && totpCode(secret, step) === code) return step;
  }
  return undefined;
}

/** The URI an authenticator app reads from a QR code. */
export function totpUri(secret: string, account: string, issuer = 'TrailCamHub'): string {
  const label = `${encodeURIComponent(issuer)}:${encodeURIComponent(account)}`;
  return `otpauth://totp/${label}?secret=${secret}&issuer=${encodeURIComponent(issuer)}`;
}
