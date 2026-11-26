import { contactOverdue, nextContactText } from './text';

/** How the hub's contact looks in the header (#431): a dot and a short word. */
export type HubLight = 'ok' | 'late' | 'silent';

export interface HubStatusLine {
  light: HubLight;
  /** "Hub verbunden", "Kontakt verspätet" … */
  title: string;
  /**
   * "nächster Kontakt gegen 21:51 Uhr", or "Kontakt überfällig seit 21:51 Uhr"
   * once that time has passed; only if the hub announced one.
   */
  next?: string;
}

const LATE_S = 5 * 60;
const SILENT_S = 30 * 60;

/**
 * The hub is on time until its announced contact is 5 minutes past, late until
 * 30 minutes past and then silent (the same threshold as the admin's fault
 * "hub not reporting", #289). Without an announcement it is judged by the last
 * contact alone: more than a day ago is silent.
 */
export function hubStatusLine(
  lastContact: number,
  nextContact: number | undefined,
  now: number,
): HubStatusLine {
  if (lastContact <= 0) return { light: 'silent', title: 'Der Hub hat sich noch nicht gemeldet' };
  const next =
    nextContact === undefined
      ? undefined
      : contactOverdue(nextContact, now)
        ? `Kontakt ${nextContactText(nextContact, now)}`
        : `nächster Kontakt ${nextContactText(nextContact, now)}`;
  const overdue = nextContact !== undefined ? now - nextContact : now - lastContact - 24 * 3600;
  if (overdue > SILENT_S) {
    return { light: 'silent', title: 'Der Hub meldet sich nicht', next };
  }
  if (overdue > LATE_S) return { light: 'late', title: 'Kontakt verspätet', next };
  return { light: 'ok', title: 'Hub verbunden', next };
}
