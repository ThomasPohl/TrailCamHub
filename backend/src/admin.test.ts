import { describe, expect, it } from 'vitest';
import { handleAdmin, isAdminEvent, registerHub } from './admin';
import { parseDeviceKey, verifySecret } from './auth/device';
import { MemoryStore } from './store/memory';

describe('isAdminEvent', () => {
  it('accepts direct invocations with an admin field', () => {
    expect(isAdminEvent({ admin: 'registerHub', hubId: 'x' })).toBe(true);
  });

  it('never treats Function URL events as admin events', () => {
    expect(isAdminEvent({ admin: 'registerHub', requestContext: { http: {} } })).toBe(false);
    expect(isAdminEvent({ rawPath: '/v1/health', requestContext: {} })).toBe(false);
    expect(isAdminEvent(null)).toBe(false);
  });
});

describe('registerHub', () => {
  it('creates a hub with a working key and links users', async () => {
    const store = new MemoryStore();
    const result = await registerHub(
      { admin: 'registerHub', hubId: 'garden-hub', name: 'Garden', users: [9047771409] },
      store,
    );
    expect(result).toMatchObject({ ok: true, created: true, linkedUsers: [9047771409] });
    if (!result.ok || !result.key) throw new Error('expected a key');

    const parsed = parseDeviceKey(result.key);
    const hub = await store.getHub('garden-hub');
    expect(parsed?.id).toBe('garden-hub');
    expect(hub?.name).toBe('Garden');
    expect(verifySecret(parsed?.secret ?? '', hub?.keyHash ?? '')).toBe(true);
    expect(await store.isLinked(9047771409, 'garden-hub')).toBe(true);
  });

  it('keeps the key of an existing hub unless asked to rotate', async () => {
    const store = new MemoryStore();
    await registerHub({ admin: 'registerHub', hubId: 'h1' }, store);
    const before = (await store.getHub('h1'))?.keyHash;

    const linked = await registerHub({ admin: 'registerHub', hubId: 'h1', users: [7] }, store);
    expect(linked).toMatchObject({ ok: true, created: false, key: undefined });
    expect((await store.getHub('h1'))?.keyHash).toBe(before);

    const rotated = await registerHub(
      { admin: 'registerHub', hubId: 'h1', rotateKey: true },
      store,
    );
    expect(rotated.ok && rotated.key).toBeTruthy();
    expect((await store.getHub('h1'))?.keyHash).not.toBe(before);
  });

  it('rejects invalid input', async () => {
    const store = new MemoryStore();
    expect(await registerHub({ admin: 'registerHub', hubId: 'bad id' }, store)).toMatchObject({
      ok: false,
    });
    expect(
      await registerHub({ admin: 'registerHub', hubId: 'h', users: [-1] }, store),
    ).toMatchObject({ ok: false });
    expect(await handleAdmin({ admin: 'dropTable' }, store)).toMatchObject({ ok: false });
  });
});
