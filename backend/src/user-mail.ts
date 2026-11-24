import { hubFaults, userFaults } from './faults';
import { USER_KEY_PREFIX, type Mailer } from './fault-mail';
import { loadHubInputs } from './hub-inputs';
import type { AppUser, Fault, HubRecord } from './model';
import type { Store } from './store/types';

/**
 * Mail to end users about their own hubs (#445). The operator mail
 * (`fault-mail.ts`) stays as it is; this adds, for users whose admin set a
 * mail address, a plain-language German mail when their hub has not checked
 * in, when a camera is not read or a battery is critical, and one mail when
 * the hub is back. Once per fault and user, like the operator mail.
 */

/**
 * A hub is only "silent" for a user this long after the check-in it planned:
 * a slow modem attach is no reason for a mail (the operator gets it after
 * `SILENT_GRACE_S`).
 */
export const USER_SILENT_GRACE_S = 2 * 60 * 60;

export interface UserMailResult {
  /** Faults that hold now and are mailed to a user. */
  current: number;
  /** Mails sent (new faults and "back again" notices). */
  sent: number;
}

/** Identifies a fault across runs for one user. */
export function userFaultKey(userId: number, hubId: string, fault: Fault): string {
  return `${USER_KEY_PREFIX}${userId}|${hubId}|${fault.kind}|${fault.camId ?? ''}`;
}

/** Whether this user wants a mail about the fault. */
function wanted(user: AppUser, fault: Fault): boolean {
  const notify = user.notify;
  if (!user.email || !notify) return false;
  switch (fault.kind) {
    case 'hubSilent':
      return notify.hubSilent && (fault.value ?? 0) > USER_SILENT_GRACE_S;
    case 'cameraNotRead':
      return notify.cameraNotRead;
    case 'hubBatteryCritical':
    case 'cameraBatteryCritical':
      return notify.battery;
    default:
      return false;
  }
}

/** "Freitag 18:30 Uhr" / "heute 18:30 Uhr", in Europe/Berlin. */
function when(at: number, now: number): string {
  const zone = { timeZone: 'Europe/Berlin' };
  const day = (t: number) => new Date(t * 1000).toLocaleDateString('de-DE', zone);
  const time = new Date(at * 1000).toLocaleTimeString('de-DE', {
    ...zone,
    hour: '2-digit',
    minute: '2-digit',
  });
  if (day(at) === day(now)) return `heute ${time} Uhr`;
  const weekday = new Date(at * 1000).toLocaleDateString('de-DE', { ...zone, weekday: 'long' });
  return `${weekday}, ${day(at)} ${time} Uhr`;
}

/** One German line for a fault of a user's hub. */
export function userFaultLine(
  fault: Fault,
  hub: Pick<HubRecord, 'name' | 'batPercent'>,
  cameraName: string,
  now: number,
): string {
  switch (fault.kind) {
    case 'hubSilent': {
      const since = fault.since ? ` seit ${when(fault.since, now)}` : '';
      const battery =
        hub.batPercent === undefined ? '' : ` Zuletzt gemeldeter Akku: ${hub.batPercent} %.`;
      return (
        `Der Hub „${hub.name}“ hat sich${since} nicht gemeldet. ` +
        `Häufige Ursachen: Akku leer, kein Netz.${battery}`
      );
    }
    case 'cameraNotRead':
      return `Die Kamera „${cameraName}“ (Hub „${hub.name}“) wurde seit einem Tag nicht gelesen, obwohl sich der Hub meldet.`;
    case 'hubBatteryCritical':
      return `Der Akku des Hubs „${hub.name}“ ist fast leer (${fault.value ?? '?'} %).`;
    case 'cameraBatteryCritical':
      return `Der Akku der Kamera „${cameraName}“ (Hub „${hub.name}“) ist fast leer (${fault.value ?? '?'} %).`;
    default:
      return `Hub „${hub.name}“: ${fault.kind}`;
  }
}

/** Collects the faults of every user's hubs and mails what is new. */
export async function runUserMail(
  store: Store,
  mailer: Mailer,
  from: string,
  now: number,
): Promise<UserMailResult> {
  const stored = await store.getNotifiedFaults();
  const operatorKeys = stored.filter((k) => !k.startsWith(USER_KEY_PREFIX));
  const known = new Set(stored.filter((k) => k.startsWith(USER_KEY_PREFIX)));

  // userId -> lines for new faults / "back again" notices, and every key that holds.
  const fresh = new Map<number, { user: AppUser; lines: string[] }>();
  const holding = new Set<string>();
  const add = (user: AppUser, line: string) => {
    const entry = fresh.get(user.userId) ?? { user, lines: [] };
    entry.lines.push(line);
    fresh.set(user.userId, entry);
  };

  const users = new Map<number, AppUser | undefined>();
  const backAgain: Array<{ user: AppUser; hub: HubRecord }> = [];
  for (const hubId of await store.listHubIds()) {
    const hub = await store.getHub(hubId);
    if (!hub) continue;
    const userIds = await store.listHubUserIds(hubId);
    const candidates: AppUser[] = [];
    for (const id of userIds) {
      if (!users.has(id)) users.set(id, await store.getAppUser(id));
      const user = users.get(id);
      if (user?.email && user.notify) candidates.push(user);
    }
    if (candidates.length === 0) continue;

    const { records, firmwareChanges, excluded } = await loadHubInputs(store, hub);
    const faults = userFaults(hubFaults(hub, records, now, firmwareChanges, excluded));
    for (const user of candidates) {
      for (const fault of faults) {
        if (!wanted(user, fault)) continue;
        const key = userFaultKey(user.userId, hubId, fault);
        holding.add(key);
        if (known.has(key)) continue;
        const cameraName =
          records.find((r) => r.camera.camId === fault.camId)?.camera.name ?? fault.camId ?? '';
        add(user, userFaultLine(fault, hub, cameraName, now));
      }
      // A silent hub that reports again: one notice, if the user still wants it.
      const silentKey = userFaultKey(user.userId, hubId, { kind: 'hubSilent', level: 'error' });
      if (known.has(silentKey) && !holding.has(silentKey) && user.notify?.hubSilent) {
        backAgain.push({ user, hub });
      }
    }
  }
  for (const { user, hub } of backAgain) {
    add(user, `Der Hub „${hub.name}“ meldet sich wieder (zuletzt ${when(hub.lastSeen, now)}).`);
  }

  let sent = 0;
  const failed = new Set<number>();
  for (const { user, lines } of fresh.values()) {
    try {
      await mailer.send({
        from,
        to: user.email as string,
        subject:
          lines.length === 1
            ? 'TrailCamHub: Hinweis zu deinem Hub'
            : `TrailCamHub: ${lines.length} Hinweise`,
        text: `${lines.map((l) => `- ${l}`).join('\n')}\n\nDiese Mails stellt der Betreiber ein; bei Fragen melde dich bei ihm.\n`,
      });
      sent += 1;
    } catch (error) {
      // Keep the faults as not notified, so the next run tries again.
      failed.add(user.userId);
      console.error(JSON.stringify({ userMailFailed: user.userId, error: String(error) }));
    }
  }
  const userOf = (key: string) => Number(key.slice(USER_KEY_PREFIX.length).split('|')[0]);
  // New faults of a user whose mail failed are tried again; so is a "back
  // again" notice (its key is no longer holding, but stays until it is sent).
  const kept = [
    ...new Set([...holding, ...[...known].filter((k) => failed.has(userOf(k)))]),
  ].filter((key) => known.has(key) || !failed.has(userOf(key)));
  const next = [...kept].sort();
  const old = [...known].sort();
  if (next.length !== old.length || next.some((k, i) => k !== old[i])) {
    await store.putNotifiedFaults([...operatorKeys, ...next], now);
  }
  return { current: holding.size, sent };
}
