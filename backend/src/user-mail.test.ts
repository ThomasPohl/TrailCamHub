import { describe, expect, it } from 'vitest';
import { createAppUser } from './admin';
import type { Mailer } from './fault-mail';
import { runFaultMail } from './fault-mail';
import type { HubRecord } from './model';
import type { Store } from './store/types';
import { HUB_ID, NOW, USER_ID, setup } from './test-utils';
import { runUserMail, USER_SILENT_GRACE_S, userFaultLine } from './user-mail';

class FakeMailer implements Mailer {
  readonly sent: Array<{ from: string; to: string; subject: string; text: string }> = [];
  failNext = false;
  async send(mail: { from: string; to: string; subject: string; text: string }) {
    if (this.failNext) {
      this.failNext = false;
      throw new Error('SES said no');
    }
    this.sent.push(mail);
  }
}

const FROM = 'hub@example.org';

async function withMail(store: Store, over: Record<string, unknown> = {}) {
  const user = await store.getAppUser(USER_ID);
  await store.putAppUser({
    ...user!,
    email: 'opa@example.org',
    notify: { hubSilent: true, cameraNotRead: true, battery: true },
    ...over,
  });
}

/** The hub planned its check-in `overdueS` seconds ago. */
async function silentFor(store: Store, overdueS: number, over: Partial<HubRecord> = {}) {
  await store.putHub({
    hubId: HUB_ID,
    name: 'Garten',
    keyHash: '',
    lastSeen: NOW - overdueS - 1800,
    batPercent: 34,
    schedule: {
      checkinIntervalS: 1800,
      nextCheckinAt: NOW - overdueS,
    } as HubRecord['schedule'],
    ...over,
  });
}

describe('mail to end users about their hub (#445)', () => {
  it('mails nobody without an address', async () => {
    const { store } = await setup();
    await silentFor(store, 5 * 3600);
    const mailer = new FakeMailer();
    expect(await runUserMail(store, mailer, FROM, NOW)).toEqual({ current: 0, sent: 0 });
    expect(mailer.sent).toHaveLength(0);
  });

  it('waits two hours before a silent hub is mailed', async () => {
    const { store } = await setup();
    await withMail(store);
    const mailer = new FakeMailer();
    await silentFor(store, USER_SILENT_GRACE_S - 60);
    expect((await runUserMail(store, mailer, FROM, NOW)).sent).toBe(0);
    await silentFor(store, USER_SILENT_GRACE_S + 60);
    expect(await runUserMail(store, mailer, FROM, NOW)).toEqual({ current: 1, sent: 1 });
    expect(mailer.sent[0]).toMatchObject({ from: FROM, to: 'opa@example.org' });
    expect(mailer.sent[0]?.text).toContain('Der Hub „Garten“ hat sich');
    expect(mailer.sent[0]?.text).toContain('Akku: 34 %');
  });

  it('mails a fault once and the return once', async () => {
    const { store } = await setup();
    await withMail(store);
    const mailer = new FakeMailer();
    await silentFor(store, 3 * 3600);
    await runUserMail(store, mailer, FROM, NOW);
    expect(await runUserMail(store, mailer, FROM, NOW + 1800)).toMatchObject({ sent: 0 });
    expect(mailer.sent).toHaveLength(1);

    // The hub checks in again.
    await store.putHub({
      hubId: HUB_ID,
      name: 'Garten',
      keyHash: '',
      lastSeen: NOW + 3600,
      schedule: { checkinIntervalS: 1800, nextCheckinAt: NOW + 5400 } as HubRecord['schedule'],
    });
    expect(await runUserMail(store, mailer, FROM, NOW + 3700)).toEqual({ current: 0, sent: 1 });
    expect(mailer.sent[1]?.text).toContain('meldet sich wieder');
    expect(await runUserMail(store, mailer, FROM, NOW + 5500)).toMatchObject({ sent: 0 });
    expect(mailer.sent).toHaveLength(2);
  });

  it('respects the switches and does not send a return notice for a switched-off kind', async () => {
    const { store } = await setup();
    await withMail(store, { notify: { hubSilent: false, cameraNotRead: true, battery: true } });
    await silentFor(store, 5 * 3600);
    const mailer = new FakeMailer();
    expect(await runUserMail(store, mailer, FROM, NOW)).toEqual({ current: 0, sent: 0 });
  });

  it('mails a critical hub battery only with the battery switch', async () => {
    const { store } = await setup();
    const mailer = new FakeMailer();
    const healthy = { lastSeen: NOW - 60, batPercent: 6, schedule: undefined };
    await store.putHub({ hubId: HUB_ID, name: 'Garten', keyHash: '', ...healthy } as HubRecord);
    await withMail(store, { notify: { hubSilent: true, cameraNotRead: true, battery: false } });
    expect((await runUserMail(store, mailer, FROM, NOW)).sent).toBe(0);
    await withMail(store);
    expect((await runUserMail(store, mailer, FROM, NOW)).sent).toBe(1);
    expect(mailer.sent[0]?.text).toContain('fast leer (6 %)');
  });

  it('tries again after a failed mail', async () => {
    const { store } = await setup();
    await withMail(store);
    await silentFor(store, 3 * 3600);
    const mailer = new FakeMailer();
    mailer.failNext = true;
    expect((await runUserMail(store, mailer, FROM, NOW)).sent).toBe(0);
    expect((await runUserMail(store, mailer, FROM, NOW + 1800)).sent).toBe(1);
  });

  it('only mails users linked to the hub, one mail per user', async () => {
    const { store } = await setup();
    await withMail(store);
    const other = await store.getAppUser(USER_ID + 1);
    await store.putAppUser({
      ...other!,
      email: 'other@example.org',
      notify: { hubSilent: true, cameraNotRead: true, battery: true },
    });
    await silentFor(store, 3 * 3600);
    const mailer = new FakeMailer();
    await runUserMail(store, mailer, FROM, NOW);
    expect(mailer.sent.map((m) => m.to)).toEqual(['opa@example.org']);
  });

  it('does not disturb the operator mail', async () => {
    const { store } = await setup();
    await withMail(store);
    await silentFor(store, 3 * 3600);
    const operator = new FakeMailer();
    const addresses = { from: FROM, to: 'admin@example.org' };
    await runFaultMail(store, operator, addresses, NOW);
    await runUserMail(store, new FakeMailer(), FROM, NOW);
    // The operator run keeps the user keys, the user run keeps the operator keys.
    await runFaultMail(store, operator, addresses, NOW + 1800);
    expect(operator.sent).toHaveLength(1);
    const again = new FakeMailer();
    await runUserMail(store, again, FROM, NOW + 1800);
    expect(again.sent).toHaveLength(0);
  });

  it('words a silent hub in plain German', () => {
    const line = userFaultLine(
      { kind: 'hubSilent', level: 'error', since: NOW - 7200 },
      { name: 'Garten', batPercent: undefined },
      '',
      NOW,
    );
    expect(line).toMatch(/^Der Hub „Garten“ hat sich seit .* nicht gemeldet\./);
    expect(line).not.toContain('Akku:');
  });
});

describe('mail address in the admin task (#445)', () => {
  const deps = async () => {
    const { store } = await setup();
    return { store, deps: { store, now: () => NOW } };
  };

  it('stores a valid address with all notices on and can remove it', async () => {
    const { store, deps: d } = await deps();
    const created = await createAppUser(
      { admin: 'createAppUser', name: 'Opa', email: ' opa@example.org ' },
      d,
    );
    expect(created.ok).toBe(true);
    const id = (created as { userId: number }).userId;
    expect(await store.getAppUser(id)).toMatchObject({
      email: 'opa@example.org',
      notify: { hubSilent: true, cameraNotRead: true, battery: true },
    });
    await createAppUser({ admin: 'createAppUser', userId: id, notify: { battery: false } }, d);
    expect((await store.getAppUser(id))?.notify).toEqual({
      hubSilent: true,
      cameraNotRead: true,
      battery: false,
    });
    await createAppUser({ admin: 'createAppUser', userId: id, email: null }, d);
    const user = await store.getAppUser(id);
    expect(user?.email).toBeUndefined();
    expect(user?.notify).toBeUndefined();
  });

  it('rejects an invalid address and unknown switches', async () => {
    const { deps: d } = await deps();
    expect(
      await createAppUser({ admin: 'createAppUser', name: 'Opa', email: 'not a mail' }, d),
    ).toMatchObject({ ok: false });
    expect(
      await createAppUser(
        { admin: 'createAppUser', name: 'Opa', email: 'a@b.de', notify: { foo: true } as never },
        d,
      ),
    ).toMatchObject({ ok: false });
  });
});
