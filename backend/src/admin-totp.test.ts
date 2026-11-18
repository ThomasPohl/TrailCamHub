import { describe, expect, it } from 'vitest';
import { createAppUser } from './admin';
import { handleRequest } from './api';
import { ADMIN_SESSION_S } from './auth/admin-session';
import { base32Decode, base32Encode, totpCode, totpUri, verifyTotp } from './auth/totp';
import type { ApiRequest, ApiResponse } from './http';
import { HUB_ID, NOW, setup } from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

// RFC 6238 test secret "12345678901234567890" (SHA-1).
const RFC_SECRET = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ';

describe('TOTP (RFC 6238)', () => {
  it('matches the RFC test vectors (last six digits)', () => {
    const at = (seconds: number) => totpCode(RFC_SECRET, Math.floor(seconds / 30));
    expect(at(59)).toBe('287082');
    expect(at(1111111109)).toBe('081804');
    expect(at(1234567890)).toBe('005924');
    expect(at(2000000000)).toBe('279037');
  });

  it('round-trips base32', () => {
    const bytes = Buffer.from('trailcam!');
    expect(base32Decode(base32Encode(bytes))?.toString()).toBe('trailcam!');
    expect(base32Decode('not base32!')).toBeUndefined();
  });

  it('accepts one step either side, refuses a replay and a wrong code', () => {
    const t = 1_700_000_000;
    const step = Math.floor(t / 30);
    expect(verifyTotp(RFC_SECRET, totpCode(RFC_SECRET, step), t)).toBe(step);
    expect(verifyTotp(RFC_SECRET, totpCode(RFC_SECRET, step - 1), t)).toBe(step - 1);
    expect(verifyTotp(RFC_SECRET, totpCode(RFC_SECRET, step + 1), t)).toBe(step + 1);
    expect(verifyTotp(RFC_SECRET, totpCode(RFC_SECRET, step - 2), t)).toBeUndefined();
    expect(verifyTotp(RFC_SECRET, totpCode(RFC_SECRET, step), t, step)).toBeUndefined();
    expect(verifyTotp(RFC_SECRET, '12345', t)).toBeUndefined();
  });

  it('builds the otpauth URI', () => {
    expect(totpUri('ABC', 'Thomas P')).toBe(
      'otpauth://totp/TrailCamHub:Thomas%20P?secret=ABC&issuer=TrailCamHub',
    );
  });
});

async function adminWorld() {
  let now = NOW;
  const world = await setup();
  world.deps.now = () => now;
  const created = await createAppUser(
    { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
    world.deps,
  );
  if (!created.ok || !created.totpSecret) throw new Error('admin needs a TOTP secret');
  const user = await createAppUser(
    { admin: 'createAppUser', name: 'Opa', hubIds: [HUB_ID] },
    world.deps,
  );
  if (!user.ok) throw new Error(user.error);
  const call = (authorization: string, method: string, path: string, data?: unknown) => {
    const req: ApiRequest = {
      method,
      path,
      headers: { authorization },
      body: data === undefined ? undefined : JSON.stringify(data),
    };
    return handleRequest(req, world.deps);
  };
  const secret = created.totpSecret;
  const elevate = async (code = totpCode(secret, Math.floor(now / 30))) => {
    const res = await call(`Bearer ${created.token}`, 'POST', '/v1/admin/elevate', { code });
    return { res, session: res.statusCode === 200 ? body<{ session: string }>(res).session : '' };
  };
  return {
    ...world,
    admin: created,
    user,
    secret,
    call,
    elevate,
    advance: (seconds: number) => (now += seconds),
    now: () => now,
  };
}

describe('admin second factor (#259)', () => {
  it('gives a new admin a secret and an otpauth URI, a user none', async () => {
    const w = await adminWorld();
    expect(w.admin.totpUri).toContain('otpauth://totp/TrailCamHub:Thomas?secret=');
    expect(w.user.totpSecret).toBeUndefined();
    expect((await w.store.getAppUser(w.user.userId))?.totpSecret).toBeUndefined();
  });

  it('treats an admin with a plain token like a user', async () => {
    const w = await adminWorld();
    const plain = `Bearer ${w.admin.token}`;
    const me = body<{ role: string; elevated: boolean; hubs: unknown[] }>(
      await w.call(plain, 'GET', '/v1/me'),
    );
    expect(me).toMatchObject({ role: 'admin', elevated: false, hubs: [] });
    const res = await w.call(plain, 'GET', '/v1/admin/hubs');
    expect(res.statusCode).toBe(403);
    expect(body<{ error: string }>(res).error).toBe('Admin session required');
  });

  it('elevates with a code and shows every hub', async () => {
    const w = await adminWorld();
    const { res, session } = await w.elevate();
    expect(res.statusCode).toBe(200);
    expect(body<{ expiresAt: number }>(res).expiresAt).toBe(NOW + ADMIN_SESSION_S);
    const me = body<{ elevated: boolean; hubs: Array<{ hubId: string }> }>(
      await w.call(`Admin ${session}`, 'GET', '/v1/me'),
    );
    expect(me.elevated).toBe(true);
    expect(me.hubs.map((h) => h.hubId)).toEqual([HUB_ID, 'other-hub']);
    expect((await w.call(`Admin ${session}`, 'GET', '/v1/admin/hubs')).statusCode).toBe(200);
  });

  it('refuses a replayed code and a wrong code, and a user without a secret', async () => {
    const w = await adminWorld();
    const code = totpCode(w.secret, Math.floor(NOW / 30));
    expect((await w.elevate(code)).res.statusCode).toBe(200);
    expect((await w.elevate(code)).res.statusCode).toBe(401);
    expect((await w.elevate('000000')).res.statusCode).toBe(401);
    const asUser = await w.call(`Bearer ${w.user.token}`, 'POST', '/v1/admin/elevate', { code });
    expect(asUser.statusCode).toBe(403);
  });

  it('locks the account after five wrong codes', async () => {
    const w = await adminWorld();
    for (let i = 0; i < 5; i++) expect((await w.elevate('000000')).res.statusCode).toBe(401);
    // Even the right code is refused during the lock ...
    expect((await w.elevate()).res.statusCode).toBe(429);
    // ... and works again after it.
    w.advance(15 * 60 + 1);
    expect((await w.elevate()).res.statusCode).toBe(200);
  });

  it('ends the session after 15 minutes', async () => {
    const w = await adminWorld();
    const { session } = await w.elevate();
    w.advance(ADMIN_SESSION_S - 1);
    expect((await w.call(`Admin ${session}`, 'GET', '/v1/admin/hubs')).statusCode).toBe(200);
    w.advance(1);
    expect((await w.call(`Admin ${session}`, 'GET', '/v1/admin/hubs')).statusCode).toBe(401);
  });

  it('ends the session when the token or the secret is rotated', async () => {
    const w = await adminWorld();
    const { session } = await w.elevate();
    await createAppUser({ admin: 'createAppUser', userId: w.admin.userId }, w.deps);
    expect((await w.call(`Admin ${session}`, 'GET', '/v1/admin/hubs')).statusCode).toBe(401);

    const again = await createAppUser(
      { admin: 'createAppUser', userId: w.admin.userId, rotateTotp: true },
      w.deps,
    );
    if (!again.ok) throw new Error(again.error);
    expect(again.totpSecret).toBeDefined();
    expect(again.totpSecret).not.toBe(w.secret);
    // The old secret no longer verifies.
    const old = await w.call(`Bearer ${again.token}`, 'POST', '/v1/admin/elevate', {
      code: totpCode(w.secret, Math.floor(w.now() / 30)),
    });
    expect(old.statusCode).toBe(401);
  });

  it('keeps the secret when only the token is rotated', async () => {
    const w = await adminWorld();
    const again = await createAppUser({ admin: 'createAppUser', userId: w.admin.userId }, w.deps);
    if (!again.ok) throw new Error(again.error);
    expect(again.totpSecret).toBeUndefined();
    expect((await w.store.getAppUser(w.admin.userId))?.totpSecret).toBe(w.secret);
  });

  it('refuses a TOTP secret for a user', async () => {
    const w = await adminWorld();
    const res = await createAppUser(
      { admin: 'createAppUser', userId: w.user.userId, rotateTotp: true },
      w.deps,
    );
    expect(res.ok).toBe(false);
  });
});

describe('rotating and revoking users by the admin (#259)', () => {
  it('rotates a user token: old token dead, new one works', async () => {
    const w = await adminWorld();
    const { session } = await w.elevate();
    const res = await w.call(`Admin ${session}`, 'POST', `/v1/admin/users/${w.user.userId}/rotate`);
    expect(res.statusCode).toBe(200);
    const rotated = body<{ token: string; name: string }>(res);
    expect(rotated.name).toBe('Opa');
    expect((await w.call(`Bearer ${w.user.token}`, 'GET', '/v1/cameras')).statusCode).toBe(401);
    expect((await w.call(`Bearer ${rotated.token}`, 'GET', '/v1/cameras')).statusCode).toBe(200);
    // The hub link survived.
    expect(await w.store.isLinked(w.user.userId, HUB_ID)).toBe(true);
  });

  it('revokes a user token', async () => {
    const w = await adminWorld();
    const { session } = await w.elevate();
    const res = await w.call(`Admin ${session}`, 'POST', `/v1/admin/users/${w.user.userId}/revoke`);
    expect(res.statusCode).toBe(200);
    expect((await w.call(`Bearer ${w.user.token}`, 'GET', '/v1/cameras')).statusCode).toBe(401);
  });

  it('needs an admin session and never touches admin accounts', async () => {
    const w = await adminWorld();
    const plain = `Bearer ${w.admin.token}`;
    const path = `/v1/admin/users/${w.user.userId}/rotate`;
    expect((await w.call(plain, 'POST', path)).statusCode).toBe(403);
    expect((await w.call(`Bearer ${w.user.token}`, 'POST', path)).statusCode).toBe(403);

    const { session } = await w.elevate();
    const adminPath = `/v1/admin/users/${w.admin.userId}`;
    expect((await w.call(`Admin ${session}`, 'POST', `${adminPath}/rotate`)).statusCode).toBe(403);
    expect((await w.call(`Admin ${session}`, 'POST', `${adminPath}/revoke`)).statusCode).toBe(403);
    const unknown = await w.call(`Admin ${session}`, 'POST', '/v1/admin/users/1/rotate');
    expect(unknown.statusCode).toBe(404);
  });

  it('writes an audit log, newest first, visible only with a session', async () => {
    const w = await adminWorld();
    await w.elevate('000000');
    w.advance(1);
    const { session } = await w.elevate();
    w.advance(1);
    await w.call(`Admin ${session}`, 'POST', `/v1/admin/users/${w.user.userId}/rotate`);
    w.advance(1);
    await w.call(`Admin ${session}`, 'POST', `/v1/admin/users/${w.user.userId}/revoke`);

    const log = body<Array<{ action: string; actor: number; target?: number }>>(
      await w.call(`Admin ${session}`, 'GET', '/v1/admin/audit'),
    );
    expect(log.map((e) => e.action)).toEqual([
      'revokeUser',
      'rotateUser',
      'elevate',
      'elevateFailed',
    ]);
    expect(log[0]).toMatchObject({ actor: w.admin.userId, target: w.user.userId });
    const plainAudit = await w.call(`Bearer ${w.admin.token}`, 'GET', '/v1/admin/audit');
    expect(plainAudit.statusCode).toBe(403);
  });
});
