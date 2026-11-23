import { hubFaults } from './faults';
import { loadHubInputs } from './hub-inputs';
import type { Fault, HubRecord } from './model';
import type { Store } from './store/types';

/**
 * Mail to the operator about new faults (#265). The admin view lists every
 * fault; this mails the ones that need action, once each: when a fault
 * appears, not at every run while it lasts. If it ends and comes back, it is
 * mailed again. Only the operator is mailed, never an end user.
 */

export interface Mailer {
  send(mail: { from: string; to: string; subject: string; text: string }): Promise<void>;
}

/** Faults worth a mail; the early warnings (low battery, nearly full) stay in the admin view. */
const MAILED: ReadonlySet<Fault['kind']> = new Set([
  'hubNeverSeen',
  'hubSilent',
  'hubBatteryCritical',
  'cameraBatteryCritical',
  'cameraBatteryLow',
  'sdMissing',
  'sdFull',
  'requestsFailed',
  'firmwareDowngrade',
  'firmwareUpdateFailed',
  'cameraNotRead',
]);

/** Identifies a fault across runs: it is the same one while hub, kind and camera match. */
export function faultKey(hubId: string, fault: Fault): string {
  return `${hubId}|${fault.kind}|${fault.camId ?? ''}`;
}

function duration(seconds: number): string {
  const minutes = Math.round(seconds / 60);
  if (minutes < 90) return `${minutes} min`;
  const hours = Math.round(minutes / 60);
  return hours < 48 ? `${hours} h` : `${Math.round(hours / 24)} days`;
}

/** One line for a fault, without the hub it belongs to. */
export function faultLine(fault: Fault, cameraName: string): string {
  const camera = `Camera ${cameraName}`;
  const detail = fault.detail ? `: ${fault.detail}` : '';
  switch (fault.kind) {
    case 'hubNeverSeen':
      return 'has never checked in';
    case 'hubSilent':
      return fault.value !== undefined
        ? `has not checked in, ${duration(fault.value)} overdue`
        : 'has not checked in';
    case 'hubBatteryCritical':
      return `battery critical (${fault.value ?? '?'} %)`;
    case 'cameraBatteryCritical':
    case 'cameraBatteryLow':
      return `${camera}: battery ${fault.kind === 'cameraBatteryCritical' ? 'critical' : 'low'} (${fault.value ?? '?'} %)`;
    case 'sdMissing':
      return `${camera}: no SD card${detail}`;
    case 'sdFull':
      return `${camera}: SD card full (${fault.value ?? '?'} MB free)`;
    case 'requestsFailed':
      return `${camera}: ${fault.value ?? 'some'} request(s) failed${detail}`;
    case 'powerModeMismatch':
      return 'battery detected but power mode is mains (no deep sleep)';
    case 'firmwareDowngrade':
      return `firmware went back to an older version (${fault.detail ?? '?'})`;
    case 'firmwareUpdateFailed':
      return `firmware update failed or was rolled back${detail}`;
    case 'cameraNotRead':
      return `${camera}: not read for a day although the hub checks in`;
    default:
      return `${fault.kind}${detail}`;
  }
}

export interface FaultMailResult {
  /** Faults that hold now and are mailed kinds. */
  current: number;
  /** Faults in the mail sent by this run. */
  sent: number;
}

/**
 * Looks at every hub, mails the faults that are new since the last run in one
 * message and remembers what holds. If sending fails, nothing is remembered,
 * so the next run tries again. Without a sender address (the feature is off)
 * use no mailer and call nothing.
 */
export async function runFaultMail(
  store: Store,
  mailer: Mailer,
  addresses: { from: string; to: string },
  now: number,
): Promise<FaultMailResult> {
  const lines = new Map<string, string>();
  for (const hubId of await store.listHubIds()) {
    const hub: HubRecord | undefined = await store.getHub(hubId);
    if (!hub) continue;
    const { records, firmwareChanges, excluded } = await loadHubInputs(store, hub);
    for (const fault of hubFaults(hub, records, now, firmwareChanges, excluded)) {
      if (!MAILED.has(fault.kind)) continue;
      const cameraName =
        records.find((r) => r.camera.camId === fault.camId)?.camera.name ?? fault.camId ?? '';
      lines.set(faultKey(hubId, fault), `${hub.name} (${hubId}): ${faultLine(fault, cameraName)}`);
    }
  }

  const known = new Set(await store.getNotifiedFaults());
  const fresh = [...lines].filter(([key]) => !known.has(key)).map(([, line]) => line);
  if (fresh.length > 0) {
    await mailer.send({
      ...addresses,
      subject: `TrailCamHub: ${fresh.length} new fault${fresh.length === 1 ? '' : 's'}`,
      text: `${fresh.map((line) => `- ${line}`).join('\n')}\n\nDetails: admin view of the gallery app.\n`,
    });
  }
  // Faults that ended are forgotten, so a return is mailed again.
  const holding = [...lines.keys()].sort();
  if (fresh.length > 0 || holding.length !== known.size || holding.some((k) => !known.has(k))) {
    await store.putNotifiedFaults(holding, now);
  }
  return { current: lines.size, sent: fresh.length };
}

/** Sends through Amazon SES (v2). */
export class SesMailer implements Mailer {
  async send(mail: { from: string; to: string; subject: string; text: string }): Promise<void> {
    // The Node.js 22 Lambda runtime ships the AWS SDK v3 with every client;
    // loading it by name keeps it out of package.json (and the lock file) and
    // out of the bundle. Tests use a fake Mailer.
    const name: string = '@aws-sdk/client-sesv2';
    const ses = (await import(name)) as {
      SESv2Client: new () => { send(command: unknown): Promise<unknown> };
      SendEmailCommand: new (input: unknown) => unknown;
    };
    await new ses.SESv2Client().send(
      new ses.SendEmailCommand({
        FromEmailAddress: mail.from,
        Destination: { ToAddresses: [mail.to] },
        Content: {
          Simple: {
            Subject: { Data: mail.subject, Charset: 'UTF-8' },
            Body: { Text: { Data: mail.text, Charset: 'UTF-8' } },
          },
        },
      }),
    );
  }
}
