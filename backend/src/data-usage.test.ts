import { describe, expect, it } from 'vitest';
import { createAppUser } from './admin';
import { createAdminSession } from './auth/admin-session';
import { handleRequest } from './api';
import type { ApiRequest, ApiResponse } from './http';
import type { DataUsageDay, HubUsage } from './model';
import { HUB_ID, hubRequest, NOW, setup } from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;
const TODAY = new Date(NOW * 1000).toISOString().slice(0, 10);

function adminRequest(session: string, method: string, path: string, data?: unknown): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `Admin ${session}` },
    body: data === undefined ? undefined : JSON.stringify(data),
  };
}

async function adminWorld() {
  const world = await setup();
  const admin = await createAppUser(
    { admin: 'createAppUser', name: 'Thomas', role: 'admin' },
    world.deps,
  );
  if (!admin.ok) throw new Error(admin.error);
  const record = await world.store.getAppUser(admin.userId);
  if (!record) throw new Error('unknown user');
  return { world, session: createAdminSession(record, NOW) };
}

describe('data usage of a hub (#415)', () => {
  it('adds the reported bytes to the sum of the day and transport', async () => {
    const { world } = await adminWorld();
    const send = (dataUsage: unknown) =>
      handleRequest(hubRequest('/v1/device/checkin', world.hubKey, { dataUsage }), world.deps);
    const first = await send([{ transport: 'modem', txBytes: 1000, rxBytes: 200 }]);
    expect(first.statusCode).toBe(200);
    await send([
      { transport: 'modem', txBytes: 500, rxBytes: 100 },
      { transport: 'wifi', txBytes: 7, rxBytes: 8 },
    ]);
    const days = await world.store.listDataUsage(HUB_ID, TODAY);
    expect(days).toEqual<DataUsageDay[]>([
      { day: TODAY, transport: 'modem', txBytes: 1500, rxBytes: 300 },
      { day: TODAY, transport: 'wifi', txBytes: 7, rxBytes: 8 },
    ]);
  });

  it('accepts check-ins from old firmware without dataUsage', async () => {
    const { world } = await adminWorld();
    const response = await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { hubBatPercent: 80 }),
      world.deps,
    );
    expect(response.statusCode).toBe(200);
    expect(await world.store.listDataUsage(HUB_ID, '2000-01-01')).toEqual([]);
  });

  it('skips malformed entries and counts one entry per transport', async () => {
    const { world } = await adminWorld();
    await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, {
        dataUsage: [
          { transport: 'satellite', txBytes: 1, rxBytes: 1 },
          { transport: 'wifi', txBytes: -5, rxBytes: 1 },
          { transport: 'wifi', txBytes: 1.5, rxBytes: 1 },
          { transport: 'wifi', txBytes: 0, rxBytes: 0 },
          { transport: 'ethernet', txBytes: 10, rxBytes: 20 },
          { transport: 'ethernet', txBytes: 99, rxBytes: 99 },
          null,
        ],
      }),
      world.deps,
    );
    expect(await world.store.listDataUsage(HUB_ID, TODAY)).toEqual([
      { day: TODAY, transport: 'ethernet', txBytes: 99, rxBytes: 99 },
    ]);
    const garbage = await handleRequest(
      hubRequest('/v1/device/checkin', world.hubKey, { dataUsage: 'lots' }),
      world.deps,
    );
    expect(garbage.statusCode).toBe(200);
  });

  it('returns the previous and the current month with the budget, for the admin only', async () => {
    const { world, session } = await adminWorld();
    // The test clock is in January 2027; keep December and one older day.
    await world.store.addDataUsage(HUB_ID, '2026-11-20', {
      transport: 'wifi',
      txBytes: 1,
      rxBytes: 1,
    });
    await world.store.addDataUsage(HUB_ID, '2026-12-15', {
      transport: 'modem',
      txBytes: 2,
      rxBytes: 3,
    });
    await world.store.addDataUsage(HUB_ID, TODAY, { transport: 'modem', txBytes: 4, rxBytes: 5 });
    const path = `/v1/admin/hubs/${HUB_ID}/usage`;
    const denied = await handleRequest(adminRequest('nope', 'GET', path), world.deps);
    expect(denied.statusCode).toBe(401);
    const response = await handleRequest(adminRequest(session, 'GET', path), world.deps);
    expect(response.statusCode).toBe(200);
    const usage = body<HubUsage>(response);
    expect(usage.budgetMB).toBeUndefined();
    expect(usage.days.map((d) => d.day)).toEqual(['2026-12-15', TODAY]);
    const unknown = await handleRequest(
      adminRequest(session, 'GET', '/v1/admin/hubs/nope/usage'),
      world.deps,
    );
    expect(unknown.statusCode).toBe(404);
  });

  it('sets and removes the monthly budget, validated', async () => {
    const { world, session } = await adminWorld();
    const path = `/v1/admin/hubs/${HUB_ID}/data-budget`;
    const post = (data: unknown) =>
      handleRequest(adminRequest(session, 'POST', path, data), world.deps);
    expect((await post({ budgetMB: 5000 })).statusCode).toBe(200);
    expect((await world.store.getHub(HUB_ID))?.dataBudgetMB).toBe(5000);
    const hubs = body<Array<{ hubId: string; dataBudgetMB?: number }>>(
      await handleRequest(adminRequest(session, 'GET', '/v1/admin/hubs'), world.deps),
    );
    expect(hubs.find((h) => h.hubId === HUB_ID)?.dataBudgetMB).toBe(5000);
    for (const bad of [0, -1, 1.5, '100', 2_000_000]) {
      expect((await post({ budgetMB: bad })).statusCode).toBe(400);
    }
    expect((await post({ budgetMB: null })).statusCode).toBe(200);
    expect((await world.store.getHub(HUB_ID))?.dataBudgetMB).toBeUndefined();
  });
});
