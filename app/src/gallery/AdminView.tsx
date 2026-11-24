import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import { ApiError } from '../api/http';
import type {
  Camera,
  PowerSetting,
  ScheduleConfig,
  ScheduleOrigin,
  ScheduleStretch,
} from '../api/types';
import { batteryForecast } from './forecast';
import { monthlyUsage } from './usage';
import { isNewer } from './release';
import {
  clockTime,
  faultText,
  forecastText,
  memoryText,
  skipReasonText,
  uplinkText,
  usageLines,
} from './text';
import type { AdminHub, AuditEntry, GalleryApi, HubHistory, HubUsage, Sample } from './types';

/**
 * The admin view (#264): every hub with its state, cameras and users, the
 * schedule and a camera read on request. For the maintainer, in the same
 * app as the gallery; users and hub links stay with `npm run add-user`.
 */

/** The hub's own report of an update (#149). */
const UPDATE_RESULT = {
  installed: 'installiert',
  rolledBack: 'zurückgerollt',
  failed: 'fehlgeschlagen',
  skipped: 'übersprungen',
} as const;

type FirmwareChangeView = NonNullable<AdminHub['firmwareChanges']>[number];

const ORIGIN: Record<ScheduleOrigin, string> = {
  'built-in': 'Standard',
  local: 'am Hub eingestellt',
  remote: 'aus der App',
};

const STRETCH: Record<ScheduleStretch, string> = {
  hubBatteryCritical: 'Hub-Akku kritisch',
  hubBatteryLow: 'Hub-Akku schwach',
  cameraBatteryLow: 'Kamera-Akku schwach',
};

const errorText = (error: unknown) => (error instanceof Error ? error.message : String(error));

const AUDIT_TEXT: Record<AuditEntry['action'], string> = {
  elevate: 'Admin angemeldet',
  elevateFailed: 'Falscher Code',
  elevateLocked: 'Falscher Code, Konto vorübergehend gesperrt',
  rotateUser: 'Neuer Link erstellt',
  revokeUser: 'Gesperrt',
  shareCreated: 'Link zum Teilen erstellt',
  shareRevoked: 'Link zum Teilen zurückgezogen',
};

/** What went wrong with a code, in words for the admin (#259). */
function codeError(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.status === 401) return 'Der Code stimmt nicht. Bitte den aktuellen Code eingeben.';
    if (error.status === 429) {
      return 'Zu viele falsche Codes. Bitte in 15 Minuten noch einmal versuchen.';
    }
    if (error.status === 403) {
      return 'Für dieses Konto ist kein zweiter Faktor eingerichtet (am Rechner: add-user --rotate-totp).';
    }
  }
  return errorText(error);
}

const needsCode = (error: unknown) =>
  error instanceof ApiError && (error.status === 401 || error.status === 403);

function minutes(seconds: number): string {
  if (seconds <= 0) return 'nur nach dem Lesen der Kamera';
  return seconds % 3600 === 0 ? `alle ${seconds / 3600} h` : `alle ${Math.round(seconds / 60)} min`;
}

function sdText(c: Camera): string {
  if (c.sdCard === 'missing') return c.sdError ? `keine Karte (${c.sdError})` : 'keine Karte';
  if (c.sdCard === 'unknown') return '–';
  const free = `${(c.sdFreeMB / 1024).toFixed(1)} GB frei`;
  return c.sdTotalMB ? `${free} von ${(c.sdTotalMB / 1024).toFixed(1)} GB` : free;
}

/**
 * The admin's second factor: the 6-digit code of the authenticator app (#259).
 * The field has the focus on arrival and the code is sent as soon as the sixth
 * digit is in, so there is no button to hunt for.
 */
function CodeForm({ api, onDone }: { api: GalleryApi; onDone: () => void }) {
  const [code, setCode] = useState('');
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  useEffect(() => {
    input.current?.focus();
  }, []);

  const send = async (value: string) => {
    if (busy || value.length !== 6) return;
    setBusy(true);
    setError(undefined);
    try {
      await api.elevate(value);
      onDone();
    } catch (e) {
      setError(codeError(e));
      setCode('');
      // Ready for the next try without a click.
      input.current?.focus();
    } finally {
      setBusy(false);
    }
  };

  const submit = (event: FormEvent) => {
    event.preventDefault();
    void send(code);
  };

  const change = (raw: string) => {
    const digits = raw.replace(/\D/g, '').slice(0, 6);
    setCode(digits);
    if (digits.length === 6) void send(digits);
  };

  return (
    <form className="g-code" onSubmit={submit} aria-busy={busy}>
      <div className="g-code-head">
        <h3>Verwaltung entsperren</h3>
        <p className="g-hint">
          Die Verwaltung ist mit einem Code aus der Authenticator-App geschützt.
        </p>
      </div>
      <label htmlFor="admin-code">Code aus der Authenticator-App</label>
      <input
        ref={input}
        id="admin-code"
        className="g-code-input"
        type="text"
        inputMode="numeric"
        pattern="[0-9]*"
        autoComplete="one-time-code"
        autoCapitalize="off"
        autoCorrect="off"
        spellCheck={false}
        maxLength={6}
        placeholder="••••••"
        readOnly={busy}
        aria-invalid={error ? true : undefined}
        aria-describedby={error ? 'admin-code-error' : undefined}
        value={code}
        onChange={(event) => change(event.target.value)}
      />
      <button type="submit" className="g-btn" disabled={busy || code.length !== 6}>
        {busy ? 'Prüfe …' : 'Anmelden'}
      </button>
      {error && (
        <p className="g-error" id="admin-code-error" role="alert">
          {error}
        </p>
      )}
    </form>
  );
}

type Confirm = { userId: number; name: string; action: 'rotate' | 'revoke' };

export function AdminView({ api, now }: { api: GalleryApi; now: () => number }) {
  const [elevated, setElevated] = useState(() => api.elevated());
  const [confirm, setConfirm] = useState<Confirm>();
  const [newLink, setNewLink] = useState<{ name: string; link: string }>();
  const [audit, setAudit] = useState<AuditEntry[]>();
  const [hubs, setHubs] = useState<AdminHub[]>();
  const [error, setError] = useState<string>();
  const [notice, setNotice] = useState<string>();
  const [busy, setBusy] = useState(false);
  const [latest, setLatest] = useState<string>();

  useEffect(() => {
    api.latestFirmware().then(setLatest, () => setLatest(undefined));
  }, [api]);

  const load = useCallback(async () => {
    if (!api.elevated()) {
      // The session ended: ask for a code again (#259).
      setElevated(false);
      return;
    }
    try {
      setHubs(await api.adminHubs());
      setError(undefined);
    } catch (e) {
      if (needsCode(e)) setElevated(false);
      else setError(errorText(e));
    }
  }, [api]);

  useEffect(() => {
    if (elevated) void load();
  }, [elevated, load]);

  const act = async (run: () => Promise<void>, success: string) => {
    setBusy(true);
    try {
      await run();
      setNotice(success);
      await load();
    } catch (e) {
      if (needsCode(e)) setElevated(false);
      else setNotice(`Das hat nicht geklappt: ${errorText(e)}`);
    } finally {
      setBusy(false);
    }
  };

  const runConfirmed = async () => {
    const c = confirm;
    if (!c) return;
    setConfirm(undefined);
    if (c.action === 'rotate') {
      await act(async () => {
        const result = await api.rotateUser(c.userId);
        const page = `${window.location.origin}${window.location.pathname}`;
        setNewLink({ name: result.name, link: `${page}#token=${result.token}` });
      }, `Neuer Link für „${c.name}“ erstellt.`);
    } else {
      await act(() => api.revokeUser(c.userId), `„${c.name}“ ist gesperrt.`);
    }
  };

  const showAudit = async () => {
    try {
      setAudit(await api.adminAudit());
    } catch (e) {
      if (needsCode(e)) setElevated(false);
      else setNotice(`Das Protokoll konnte nicht geladen werden: ${errorText(e)}`);
    }
  };

  if (!elevated) {
    return (
      <section className="g-admin" aria-label="Verwaltung">
        <CodeForm api={api} onDone={() => setElevated(true)} />
      </section>
    );
  }

  const t = now();
  const userName = (id?: number) =>
    hubs?.flatMap((h) => h.users).find((u) => u.userId === id)?.name ?? `User ${id}`;

  return (
    <section className="g-admin" aria-label="Verwaltung">
      {notice && (
        <p className="g-notice" role="status">
          {notice}
          <button type="button" className="g-link" onClick={() => setNotice(undefined)}>
            Ausblenden
          </button>
        </p>
      )}
      {confirm && (
        <div className="g-confirm" role="alertdialog" aria-label="Bestätigen">
          <p>
            {confirm.action === 'rotate'
              ? `Neuen Link für „${confirm.name}“ erstellen? Der alte Link funktioniert dann nicht mehr.`
              : `„${confirm.name}“ sperren? Der Link funktioniert dann nicht mehr, bis ein neuer erstellt wird.`}
          </p>
          <button
            type="button"
            className="g-btn g-btn--danger"
            disabled={busy}
            onClick={() => void runConfirmed()}
          >
            {confirm.action === 'rotate' ? 'Neuen Link erstellen' : 'Sperren'}
          </button>
          <button
            type="button"
            className="g-btn g-btn--quiet"
            onClick={() => setConfirm(undefined)}
          >
            Abbrechen
          </button>
        </div>
      )}
      {newLink && (
        <div className="g-newlink" role="group" aria-label="Neuer Link">
          <p>
            Neuer Einrichtungslink für „{newLink.name}“. Er wird nur jetzt angezeigt; bitte privat
            weitergeben, zum Beispiel per Signal. Der alte Link funktioniert nicht mehr.
          </p>
          <input
            type="text"
            readOnly
            aria-label="Neuer Einrichtungslink"
            value={newLink.link}
            onFocus={(event) => event.target.select()}
          />
          <button
            type="button"
            className="g-btn g-btn--small"
            onClick={() =>
              void navigator.clipboard?.writeText(newLink.link).then(
                () => setNotice('Link kopiert.'),
                () => setNotice('Kopieren ging nicht; bitte den Link von Hand markieren.'),
              )
            }
          >
            Link kopieren
          </button>
          <button
            type="button"
            className="g-btn g-btn--small g-btn--quiet"
            onClick={() => setNewLink(undefined)}
          >
            Link ausblenden
          </button>
        </div>
      )}
      {error && (
        <div className="g-error" role="alert">
          <p>Die Hubs konnten nicht geladen werden: {error}</p>
          <button type="button" className="g-btn" onClick={() => void load()}>
            Nochmal versuchen
          </button>
        </div>
      )}
      {hubs === undefined && !error && <p className="g-hint">Hubs werden geladen …</p>}
      {hubs && hubs.length === 0 && <p className="g-hint">Noch kein Hub angemeldet.</p>}
      {hubs && hubs.length > 0 && <FaultSummary hubs={hubs} />}
      {hubs?.map((hub) => (
        <article key={hub.hubId} className="g-hub" aria-label={`Hub ${hub.name}`}>
          <h2>
            Hub „{hub.name}“{hub.name !== hub.hubId && <small> ({hub.hubId})</small>}
          </h2>
          {hub.faults.length > 0 && (
            <ul className="g-faults" aria-label={`Störungen ${hub.name}`}>
              {hub.faults.map((f, i) => (
                <li key={i} className={`g-fault g-fault--${f.level}`}>
                  {f.level === 'error' ? 'Störung: ' : 'Hinweis: '}
                  {faultText(f, hub.cameras.find((c) => c.camId === f.camId)?.name, t)}
                </li>
              ))}
            </ul>
          )}
          <dl className="g-facts">
            <dt>Letzter Kontakt</dt>
            <dd>{hub.lastSeen > 0 ? clockTime(hub.lastSeen, t) : 'noch nie'}</dd>
            {hub.schedule?.nextCheckinAt !== undefined && (
              <>
                <dt>Nächster Kontakt</dt>
                <dd>gegen {clockTime(hub.schedule.nextCheckinAt, t)}</dd>
              </>
            )}
            <dt>Akku</dt>
            <dd>{hub.batPercent !== undefined ? `${hub.batPercent} %` : '–'}</dd>
            {hub.uplink && (
              <>
                <dt>Verbindung</dt>
                <dd>{uplinkText(hub.uplink, hub.modemSignalDbm, hub.modemOperator)}</dd>
              </>
            )}
            {hub.memory && (
              <>
                <dt>Speicher</dt>
                <dd>{memoryText(hub.memory)}</dd>
              </>
            )}
            <dt>Firmware</dt>
            <dd>
              {hub.firmware ?? '–'}
              {isNewer(latest, hub.firmware) && (
                <span className="g-update-available">
                  {' '}
                  · Update verfügbar: {latest} (installiert der Hub selbst beim nächsten Abruf)
                </span>
              )}
            </dd>
            {hub.firmwareChanges && hub.firmwareChanges.length > 0 && (
              <>
                <dt>Firmware-Wechsel</dt>
                <dd>
                  <FirmwareHistory hubName={hub.name} changes={hub.firmwareChanges} now={t} />
                </dd>
              </>
            )}
            {hub.schedule && (
              <>
                <dt>Kamera lesen</dt>
                <dd>
                  {hub.schedule.cameraPlan} ({ORIGIN[hub.schedule.cameraPlanOrigin]})
                </dd>
                <dt>Kontakt</dt>
                <dd>
                  {`${minutes(hub.schedule.checkinIntervalS)} (${ORIGIN[hub.schedule.checkinOrigin]})`}
                  {hub.schedule.sleeps && ', schläft dazwischen'}
                </dd>
                {hub.schedule.stretch && (
                  <>
                    <dt>Gestreckt</dt>
                    <dd>{STRETCH[hub.schedule.stretch]}</dd>
                  </>
                )}
              </>
            )}
            <dt>Nutzer</dt>
            <dd>
              {hub.users.length === 0 ? (
                'keine'
              ) : (
                <ul className="g-users" aria-label={`Nutzer ${hub.name}`}>
                  {hub.users.map((u) => (
                    <li key={u.userId}>
                      {u.name
                        ? `${u.name}${u.role === 'admin' ? ' (Admin)' : ''}`
                        : `User ${u.userId}`}
                      {u.role === 'user' && u.name && (
                        <span className="g-user-actions">
                          <button
                            type="button"
                            className="g-btn g-btn--small g-btn--quiet"
                            disabled={busy}
                            onClick={() =>
                              setConfirm({ userId: u.userId, name: u.name ?? '', action: 'rotate' })
                            }
                          >
                            Neuer Link
                          </button>
                          <button
                            type="button"
                            className="g-btn g-btn--small g-btn--quiet g-btn--danger-text"
                            disabled={busy}
                            onClick={() =>
                              setConfirm({ userId: u.userId, name: u.name ?? '', action: 'revoke' })
                            }
                          >
                            Sperren
                          </button>
                        </span>
                      )}
                    </li>
                  ))}
                </ul>
              )}
            </dd>
          </dl>

          <table className="g-table">
            <caption>Kameras</caption>
            <thead>
              <tr>
                <th scope="col">Name</th>
                <th scope="col">Modell</th>
                <th scope="col">Akku</th>
                <th scope="col">SD-Karte</th>
                <th scope="col">Temp.</th>
                <th scope="col">Zuletzt gelesen</th>
                <th scope="col">
                  <span className="g-visually-hidden">Aktion</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {hub.cameras.length === 0 && (
                <tr>
                  <td colSpan={7}>Noch keine Kamera gemeldet.</td>
                </tr>
              )}
              {hub.cameras.map((c) => (
                <tr key={c.camId}>
                  <th scope="row">
                    {c.name}
                    {c.name !== c.camId && <small> {c.camId}</small>}
                    {c.hidden && <small> (ausgeblendet)</small>}
                  </th>
                  <td>
                    {c.model} · {c.firmware}
                  </td>
                  <td>
                    {c.batPercent} %{c.isCharging ? ' (lädt)' : ''}
                  </td>
                  <td>{sdText(c)}</td>
                  <td>{c.temperature !== undefined ? `${c.temperature} °C` : '–'}</td>
                  <td>{c.lastSeen > 0 ? clockTime(c.lastSeen, t) : 'noch nie'}</td>
                  <td>
                    {c.readRequested ? (
                      'Lesen angefordert'
                    ) : (
                      <button
                        type="button"
                        className="g-btn g-btn--plain"
                        disabled={busy}
                        onClick={() =>
                          void act(
                            () => api.readCamera(c.camId),
                            `Kamera „${c.name}“ wird beim nächsten Kontakt gelesen.`,
                          )
                        }
                      >
                        Jetzt lesen
                      </button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>

          <HistoryPanel api={api} hubId={hub.hubId} now={t} />
          <UsagePanel api={api} hubId={hub.hubId} now={t} />

          <ScheduleForm
            key={`${hub.hubId}:${hub.scheduleConfig.cameraPlan ?? ''}:${hub.scheduleConfig.checkinIntervalS ?? ''}:${hub.scheduleConfig.powerSetting ?? ''}`}
            hub={hub}
            disabled={busy}
            onSave={(config) =>
              act(
                () => api.setSchedule(hub.hubId, config),
                'Der Hub übernimmt den Plan beim nächsten Kontakt.',
              )
            }
          />
        </article>
      ))}
      <section className="g-audit" aria-label="Protokoll">
        <h2>Protokoll</h2>
        {audit === undefined ? (
          <button
            type="button"
            className="g-btn g-btn--small g-btn--quiet"
            onClick={() => void showAudit()}
          >
            Protokoll anzeigen
          </button>
        ) : audit.length === 0 ? (
          <p className="g-hint">Noch nichts protokolliert.</p>
        ) : (
          <ul className="g-audit-list">
            {audit.map((entry, i) => (
              <li key={i}>
                {clockTime(entry.at, t)} · {AUDIT_TEXT[entry.action]}
                {entry.target !== undefined && ` (${userName(entry.target)})`}
              </li>
            ))}
          </ul>
        )}
      </section>
    </section>
  );
}

/** One firmware event as a line: "v1.2.0 → v1.3.0 (15.11. 05:12), installiert" (#149). */
function changeText(change: FirmwareChangeView, now: number): string {
  const result = change.result ? `, ${UPDATE_RESULT[change.result]}` : '';
  const reason = change.reason
    ? `: ${change.result === 'skipped' ? skipReasonText(change.reason) : change.reason}`
    : '';
  return `${change.from} → ${change.to} (${clockTime(change.at, now)})${result}${reason}`;
}

/** The latest firmware event; the last five (#149) on tap. */
function FirmwareHistory({
  hubName,
  changes,
  now,
}: {
  hubName: string;
  changes: FirmwareChangeView[];
  now: number;
}) {
  const [open, setOpen] = useState(false);
  const [latest] = changes;
  if (!latest) return null;
  return (
    <>
      <span>{changeText(latest, now)}</span>
      {changes.length > 1 && (
        <>
          {' '}
          <button
            type="button"
            className="g-btn g-btn--small g-btn--quiet"
            aria-expanded={open}
            onClick={() => setOpen(!open)}
          >
            {open ? 'Verlauf ausblenden' : `Verlauf (${changes.length})`}
          </button>
          {open && (
            <ul className="g-plain-list" aria-label={`Firmware-Verlauf ${hubName}`}>
              {changes.map((c) => (
                <li key={`${c.at}-${c.to}-${c.result ?? ''}`}>{changeText(c, now)}</li>
              ))}
            </ul>
          )}
        </>
      )}
    </>
  );
}

/** Battery history of the last 30 days, loaded on request (#76). */
function HistoryPanel({ api, hubId, now }: { api: GalleryApi; hubId: string; now: number }) {
  const [history, setHistory] = useState<HubHistory>();
  const [error, setError] = useState<string>();
  const [open, setOpen] = useState(false);

  const toggle = async () => {
    if (open) {
      setOpen(false);
      return;
    }
    setOpen(true);
    try {
      setHistory(await api.adminHistory(hubId));
      setError(undefined);
    } catch (e) {
      setError(errorText(e));
    }
  };

  return (
    <div className="g-history">
      <button
        type="button"
        className="g-btn g-btn--plain"
        aria-expanded={open}
        onClick={() => void toggle()}
      >
        {open ? 'Akku-Verlauf ausblenden' : 'Akku-Verlauf (30 Tage)'}
      </button>
      {open && error && <p className="g-error">Verlauf nicht geladen: {error}</p>}
      {open && history && (
        <div className="g-history__charts">
          <BatteryChart label="Hub" samples={history.hub} now={now} />
          {history.cameras.map((c) => (
            <BatteryChart
              key={c.camId}
              label={`Kamera „${c.name}“`}
              samples={c.samples}
              now={now}
            />
          ))}
        </div>
      )}
    </div>
  );
}

/** Data volume of the month with a forecast and the optional plan size (#415). */
function UsagePanel({ api, hubId, now }: { api: GalleryApi; hubId: string; now: number }) {
  const [usage, setUsage] = useState<HubUsage>();
  const [error, setError] = useState<string>();
  const [open, setOpen] = useState(false);
  const [budget, setBudget] = useState('');

  const load = async () => {
    try {
      const loaded = await api.adminUsage(hubId);
      setUsage(loaded);
      setBudget(loaded.budgetMB !== undefined ? String(loaded.budgetMB) : '');
      setError(undefined);
    } catch (e) {
      setError(errorText(e));
    }
  };
  const toggle = async () => {
    setOpen(!open);
    if (!open) await load();
  };
  const save = async (value: number | undefined) => {
    try {
      await api.setDataBudget(hubId, value);
      await load();
    } catch (e) {
      setError(errorText(e));
    }
  };
  const parsed = Number(budget);
  const valid = Number.isInteger(parsed) && parsed >= 1 && parsed <= 1_000_000;

  return (
    <div className="g-history">
      <button
        type="button"
        className="g-btn g-btn--plain"
        aria-expanded={open}
        onClick={() => void toggle()}
      >
        {open ? 'Datenmenge ausblenden' : 'Datenmenge (Monat)'}
      </button>
      {open && error && <p className="g-error">Datenmenge nicht geladen: {error}</p>}
      {open && usage && (
        <div className="g-usage">
          {usageLines(monthlyUsage(usage.days, now, usage.budgetMB), now).map((line) => (
            <p key={line} className="g-chart__forecast">
              {line}
            </p>
          ))}
          <label>
            Kontingent pro Monat (MB, leer = keins){' '}
            <input
              type="number"
              min={1}
              max={1000000}
              value={budget}
              onChange={(e) => setBudget(e.target.value)}
            />
          </label>{' '}
          <button
            type="button"
            className="g-btn"
            disabled={!valid}
            onClick={() => void save(parsed)}
          >
            Speichern
          </button>{' '}
          {usage.budgetMB !== undefined && (
            <button
              type="button"
              className="g-btn g-btn--plain"
              onClick={() => void save(undefined)}
            >
              Entfernen
            </button>
          )}
        </div>
      )}
    </div>
  );
}

const CHART_W = 320;
const CHART_H = 64;

/** A small line of the battery level, 0-100 %, over the samples' time span. */
export function BatteryChart({
  label,
  samples,
  now,
}: {
  label: string;
  samples: Sample[];
  now: number;
}) {
  const points = samples.filter((s) => s.batPercent !== undefined);
  const first = points[0];
  const last = points[points.length - 1];
  if (!first || !last) {
    return (
      <figure className="g-chart">
        <figcaption>{label}: noch keine Werte</figcaption>
      </figure>
    );
  }
  const span = Math.max(1, last.at - first.at);
  // Values taken while charging say little about the level (#421): the line
  // skips them and a tick marks where they were.
  const x = (at: number) => ((at - first.at) / span) * CHART_W;
  const y = (percent: number) => CHART_H - (percent / 100) * CHART_H;
  const charging = (s: Sample) => s.charging ?? (s.batPercent ?? 0) >= 100;
  const lines: string[] = [];
  let current: string[] = [];
  for (const s of points) {
    if (charging(s)) {
      if (current.length > 0) lines.push(current.join(' '));
      current = [];
    } else {
      current.push(`${x(s.at).toFixed(1)},${y(s.batPercent ?? 0).toFixed(1)}`);
    }
  }
  if (current.length > 0) lines.push(current.join(' '));
  const ticks = points.filter(charging).map((s) => x(s.at));
  const summary = `${label}: Akku ${first.batPercent} % → ${last.batPercent} %`;
  const forecast = batteryForecast(samples);
  return (
    <figure className="g-chart">
      <figcaption>{summary}</figcaption>
      {forecast && <p className="g-chart__forecast">{forecastText(forecast, now)}</p>}
      <svg
        viewBox={`-2 -2 ${CHART_W + 4} ${CHART_H + 4}`}
        width={CHART_W}
        height={CHART_H}
        role="img"
        aria-label={summary}
      >
        <line
          x1={0}
          y1={CHART_H * 0.75}
          x2={CHART_W}
          y2={CHART_H * 0.75}
          className="g-chart__low"
        />
        {ticks.map((tx) => (
          <line key={tx} x1={tx} y1={0} x2={tx} y2={CHART_H} className="g-chart__charging" />
        ))}
        {lines.map((xy) => (
          <polyline key={xy} points={xy} className="g-chart__line" />
        ))}
      </svg>
    </figure>
  );
}

/** One line over all hubs, so a fault is seen without scrolling (#265). */
function FaultSummary({ hubs }: { hubs: AdminHub[] }) {
  const errors = hubs.flatMap((h) => h.faults).filter((f) => f.level === 'error').length;
  const warnings = hubs.flatMap((h) => h.faults).length - errors;
  if (errors + warnings === 0) {
    return <p className="g-notice g-notice--ok">Keine Störungen.</p>;
  }
  const parts = [
    errors > 0 && (errors === 1 ? '1 Störung' : `${errors} Störungen`),
    warnings > 0 && (warnings === 1 ? '1 Hinweis' : `${warnings} Hinweise`),
  ].filter(Boolean);
  return (
    <p className={errors > 0 ? 'g-error' : 'g-banner'} role="status">
      {parts.join(' und ')} – Einzelheiten beim jeweiligen Hub.
    </p>
  );
}

/** The schedule set in the app; empty fields leave the setting to the hub. */
function ScheduleForm({
  hub,
  disabled,
  onSave,
}: {
  hub: AdminHub;
  disabled: boolean;
  onSave(config: ScheduleConfig): Promise<void>;
}) {
  const [plan, setPlan] = useState(hub.scheduleConfig.cameraPlan ?? '');
  const [interval, setIntervalText] = useState(
    hub.scheduleConfig.checkinIntervalS ? String(hub.scheduleConfig.checkinIntervalS / 60) : '',
  );
  const [power, setPower] = useState<PowerSetting | ''>(hub.scheduleConfig.powerSetting ?? '');
  const minutesValue = interval.trim() === '' ? undefined : Number(interval);
  const invalid =
    minutesValue !== undefined &&
    !(Number.isInteger(minutesValue) && minutesValue >= 5 && minutesValue <= 1440);
  const planId = `g-plan-${hub.hubId}`;
  const intervalId = `g-interval-${hub.hubId}`;
  const powerId = `g-power-${hub.hubId}`;

  return (
    <form
      className="g-schedule"
      onSubmit={(event) => {
        event.preventDefault();
        if (invalid) return;
        const config: ScheduleConfig = {};
        if (plan.trim()) config.cameraPlan = plan.trim();
        if (minutesValue !== undefined) config.checkinIntervalS = minutesValue * 60;
        if (power !== '') config.powerSetting = power;
        void onSave(config);
      }}
    >
      <h3>Weckplan aus der App</h3>
      <p className="g-hint">Leere Felder: der Hub nimmt seine eigene Einstellung.</p>
      <label htmlFor={planId}>Kamera lesen (z. B. 05:00-09:00=30,*=120)</label>
      <input
        id={planId}
        type="text"
        value={plan}
        placeholder={hub.schedule?.cameraPlan ?? ''}
        autoComplete="off"
        onChange={(event) => setPlan(event.target.value)}
      />
      <label htmlFor={intervalId}>Kontakt alle … Minuten (5–1440)</label>
      <input
        id={intervalId}
        type="number"
        min={5}
        max={1440}
        value={interval}
        aria-invalid={invalid}
        onChange={(event) => setIntervalText(event.target.value)}
      />
      <label htmlFor={powerId}>Stromversorgung</label>
      <select
        id={powerId}
        value={power}
        onChange={(event) => setPower(event.target.value as PowerSetting | '')}
      >
        <option value="">Der Hub behält seine Einstellung</option>
        <option value="auto">Automatisch (Akku erkennen)</option>
        <option value="mains">Netzbetrieb (kein Tiefschlaf)</option>
        <option value="battery">Akkubetrieb (Tiefschlaf)</option>
      </select>
      <p className="g-hint">
        Der Hub speichert die Einstellung, wenn sein Board sie kann, und nutzt sie ab dem nächsten
        Start. Akkubetrieb am Netz-Hub: Er ist dann nur noch zu den Kontaktzeiten erreichbar.
      </p>
      <div className="g-card__actions">
        <button type="submit" className="g-btn" disabled={disabled || invalid}>
          Übernehmen
        </button>
      </div>
    </form>
  );
}
