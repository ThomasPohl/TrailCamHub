import { describe, expect, it } from 'vitest';
import { faultKey, faultLine, runFaultMail, type Mailer } from './fault-mail';
import type { Fault, HubRecord } from './model';
import { HUB_ID, NOW, setup } from './test-utils';
import type { Store } from './store/types';

const ADDRESSES = { from: 'hub@example.org', to: 'admin@example.org' };

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

const hubRecord = (hubId: string, over: Partial<HubRecord> = {}): HubRecord => ({
  hubId,
  name: hubId === HUB_ID ? 'Garden hub' : 'Other',
  keyHash: '',
  lastSeen: NOW - 60,
  batPercent: 80,
  ...over,
});

/** Both hubs of setup() checked in a minute ago and are fine. */
async function healthy(store: Store) {
  await store.putHub(hubRecord(HUB_ID));
  await store.putHub(hubRecord('other-hub'));
}

describe('mail about faults (#265)', () => {
  it('mails a new fault once, not at every run', async () => {
    const { store } = await setup();
    const mailer = new FakeMailer();
    // Neither hub has ever checked in.
    expect(await runFaultMail(store, mailer, ADDRESSES, NOW)).toEqual({ current: 2, sent: 2 });
    expect(mailer.sent).toHaveLength(1);
    expect(mailer.sent[0]).toMatchObject({ ...ADDRESSES, subject: 'TrailCamHub: 2 new faults' });
    expect(mailer.sent[0]?.text).toContain('- Garden hub (garden-hub): has never checked in');
    expect(mailer.sent[0]?.text).toContain('- Other (other-hub): has never checked in');

    expect(await runFaultMail(store, mailer, ADDRESSES, NOW + 1800)).toEqual({
      current: 2,
      sent: 0,
    });
    expect(mailer.sent).toHaveLength(1);
  });

  it('mails a fault again when it came back, and only the new one', async () => {
    const { store } = await setup();
    const mailer = new FakeMailer();
    await runFaultMail(store, mailer, ADDRESSES, NOW);

    await healthy(store);
    expect(await runFaultMail(store, mailer, ADDRESSES, NOW + 60)).toEqual({
      current: 0,
      sent: 0,
    });
    // The garden hub is two hours late (a day plus two); the other one stays fine.
    await store.putHub(hubRecord(HUB_ID, { lastSeen: NOW - 26 * 3600 }));
    await runFaultMail(store, mailer, ADDRESSES, NOW + 120);
    expect(mailer.sent).toHaveLength(2);
    expect(mailer.sent[1]?.subject).toBe('TrailCamHub: 1 new fault');
    expect(mailer.sent[1]?.text).toContain(
      'Garden hub (garden-hub): has not checked in, 2 h overdue',
    );
    expect(mailer.sent[1]?.text).not.toContain('Other');
  });

  it('leaves the early warnings to the admin view', async () => {
    const { store } = await setup();
    const mailer = new FakeMailer();
    await healthy(store);
    // 20 %: low, not critical.
    await store.putHub(hubRecord(HUB_ID, { batPercent: 20 }));
    expect(await runFaultMail(store, mailer, ADDRESSES, NOW)).toEqual({ current: 0, sent: 0 });
    expect(mailer.sent).toEqual([]);
    await store.putHub(hubRecord(HUB_ID, { batPercent: 8 }));
    await runFaultMail(store, mailer, ADDRESSES, NOW + 60);
    expect(mailer.sent).toHaveLength(1);
    expect(mailer.sent[0]?.text).toContain('battery critical (8 %)');
  });

  it('tries again at the next run when the mail could not be sent', async () => {
    const { store } = await setup();
    const mailer = new FakeMailer();
    mailer.failNext = true;
    await expect(runFaultMail(store, mailer, ADDRESSES, NOW)).rejects.toThrow('SES said no');
    expect(await store.getNotifiedFaults()).toEqual([]);
    expect(await runFaultMail(store, mailer, ADDRESSES, NOW + 1800)).toEqual({
      current: 2,
      sent: 2,
    });
    expect(mailer.sent).toHaveLength(1);
  });

  it('words each fault and keeps hub, kind and camera in its key', () => {
    const fault = (f: Partial<Fault> & Pick<Fault, 'kind'>): Fault => ({ level: 'error', ...f });
    expect(faultKey('h1', fault({ kind: 'sdFull', camId: 'C1' }))).toBe('h1|sdFull|C1');
    expect(faultKey('h1', fault({ kind: 'hubSilent' }))).toBe('h1|hubSilent|');
    expect(faultLine(fault({ kind: 'sdFull', camId: 'C1', value: 120 }), 'Wiese')).toBe(
      'Camera Wiese: SD card full (120 MB free)',
    );
    expect(faultLine(fault({ kind: 'cameraBatteryLow', camId: 'C1', value: 22 }), 'Wiese')).toBe(
      'Camera Wiese: battery low (22 %)',
    );
    expect(
      faultLine(
        fault({ kind: 'requestsFailed', camId: 'C1', value: 2, detail: 'timeout' }),
        'Wiese',
      ),
    ).toBe('Camera Wiese: 2 request(s) failed: timeout');
    expect(
      faultLine(
        fault({ kind: 'firmwareUpdateFailed', detail: 'v0.1.12 → v0.1.13: image check' }),
        '',
      ),
    ).toBe('firmware update failed or was rolled back: v0.1.12 → v0.1.13: image check');
    expect(faultLine(fault({ kind: 'hubSilent', value: 4 * 3600 }), '')).toBe(
      'has not checked in, 4 h overdue',
    );
  });
});
