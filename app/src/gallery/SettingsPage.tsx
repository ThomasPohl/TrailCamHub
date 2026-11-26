import { useEffect, useMemo, useState } from 'react';
import type { HubSchedule, ScheduleConfig, ScheduleOrigin } from '../api/types';
import { BatteryChart } from './AdminView';
import { quietHours } from './forecast';
import {
  boundWords,
  clockText,
  CONTACT_STEPS,
  daySegments,
  DEFAULT_SUN,
  everyText,
  formatEditPlan,
  normalizeLocation,
  parseEditPlan,
  parseQuietText,
  PRESETS,
  readsPerDay,
  type EditPlan,
  type SunTimes,
} from './schedulePlan';
import { clockTime, faultText, usageLines } from './text';
import type { Fault, GalleryApi, GalleryStatus, HubHistory, HubUsage } from './types';
import { monthlyUsage } from './usage';

/**
 * The user's settings (#431): cameras, the wake-up schedule, battery and data,
 * hints. Opened from the gear in the gallery header; the admin's technical
 * views stay in "Verwaltung".
 */

export type SettingsTab = 'cameras' | 'schedule' | 'battery' | 'hints';

const TABS: Array<{ id: SettingsTab; label: string }> = [
  { id: 'cameras', label: 'Kameras' },
  { id: 'schedule', label: 'Zeitplan' },
  { id: 'battery', label: 'Akku & Daten' },
  { id: 'hints', label: 'Hinweise' },
];

const errorText = (error: unknown) => (error instanceof Error ? error.message : String(error));

export function SettingsPage({
  api,
  status,
  now,
  initialTab = 'cameras',
  onCameraSettings,
  onManageCameras,
  onChanged,
}: {
  api: GalleryApi;
  status: GalleryStatus;
  now: number;
  initialTab?: SettingsTab;
  /** Opens the camera settings dialog (#368) for a camera. */
  onCameraSettings(camId: string): void;
  /** Opens "Kameras verwalten" (rename, hide, found cameras). */
  onManageCameras(): void;
  /** Something changed at the backend: reload the status. */
  onChanged(): void;
}) {
  const [tab, setTab] = useState<SettingsTab>(initialTab);
  const hub = status.hub;

  return (
    <main className="g-main g-settings">
      <h2>Einstellungen</h2>
      <div className="g-tabs" role="tablist" aria-label="Einstellungen">
        {TABS.map((t) => (
          <button
            key={t.id}
            type="button"
            role="tab"
            id={`g-tab-${t.id}`}
            aria-selected={tab === t.id}
            aria-controls={`g-panel-${t.id}`}
            className={tab === t.id ? 'g-tab g-tab--active' : 'g-tab'}
            onClick={() => setTab(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>
      <section
        className="g-settings__panel"
        role="tabpanel"
        id={`g-panel-${tab}`}
        aria-labelledby={`g-tab-${tab}`}
      >
        {!hub && tab !== 'cameras' ? (
          <p className="g-hint">Noch kein Hub hat sich gemeldet.</p>
        ) : tab === 'cameras' ? (
          <CamerasTab
            api={api}
            status={status}
            now={now}
            onCameraSettings={onCameraSettings}
            onManageCameras={onManageCameras}
          />
        ) : tab === 'schedule' && hub ? (
          <ScheduleEditor
            key={hub.hubId}
            api={api}
            hubId={hub.hubId}
            config={hub.config}
            schedule={hub.schedule}
            cameraCount={status.cameras.length}
            nextContact={status.nextContact}
            now={now}
            onSaved={onChanged}
          />
        ) : tab === 'battery' && hub ? (
          <BatteryTab api={api} hubId={hub.hubId} schedule={hub.schedule} now={now} />
        ) : hub ? (
          <HintsTab api={api} hubId={hub.hubId} status={status} now={now} />
        ) : null}
      </section>
    </main>
  );
}

function Meter({ percent, label }: { percent: number | undefined; label: string }) {
  if (percent === undefined) return <span className="g-hint">–</span>;
  const value = Math.max(0, Math.min(100, Math.round(percent)));
  const tone = value < 20 ? 'g-meter--low' : value < 40 ? 'g-meter--mid' : '';
  return (
    <span className={`g-meter ${tone}`} title={`${label}: ${value} %`}>
      <span className="g-meter__bar" style={{ width: `${value}%` }} />
      <span className="g-meter__text">{value} %</span>
    </span>
  );
}

function CamerasTab({
  api,
  status,
  now,
  onCameraSettings,
  onManageCameras,
}: {
  api: GalleryApi;
  status: GalleryStatus;
  now: number;
  onCameraSettings(camId: string): void;
  onManageCameras(): void;
}) {
  const [requested, setRequested] = useState<Set<string>>(new Set());
  const [error, setError] = useState<string>();
  const read = async (camId: string) => {
    try {
      await api.readCamera(camId);
      setRequested((s) => new Set(s).add(camId));
      setError(undefined);
    } catch (e) {
      setError(errorText(e));
    }
  };
  const readAll = async () => {
    for (const c of status.cameras.filter((c) => !c.hidden)) await read(c.camId);
  };

  if (status.cameras.length === 0) {
    return (
      <>
        <p className="g-hint">Noch keine Kamera.</p>
        <button type="button" className="g-btn" onClick={onManageCameras}>
          Kameras verwalten
        </button>
      </>
    );
  }
  const next = status.nextContact;
  return (
    <>
      <table className="g-table">
        <thead>
          <tr>
            <th>Kamera</th>
            <th>Akku</th>
            <th>SD-Karte frei</th>
            <th>Zuletzt gelesen</th>
            <th>
              <span className="g-sr">Aktionen</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {status.cameras.map((c) => {
            const sdFree =
              c.sdFreeMB !== undefined && c.sdTotalMB
                ? (c.sdFreeMB / c.sdTotalMB) * 100
                : undefined;
            return (
              <tr key={c.camId} className={c.hidden ? 'g-table__hidden' : undefined}>
                <td>
                  {c.name}
                  {c.hidden && <span className="g-hint"> (ausgeblendet)</span>}
                </td>
                <td>
                  <Meter percent={c.batPercent} label="Akku" />
                </td>
                <td>
                  {c.sdCard === 'missing' ? (
                    <span className="g-error">keine Karte</span>
                  ) : (
                    <Meter percent={sdFree} label="SD-Karte frei" />
                  )}
                </td>
                <td>{c.lastRead ? clockTime(c.lastRead, now) : 'noch nie'}</td>
                <td className="g-table__actions">
                  <button
                    type="button"
                    className="g-btn g-btn--plain"
                    disabled={requested.has(c.camId)}
                    onClick={() => void read(c.camId)}
                  >
                    {requested.has(c.camId) ? 'Angefordert' : 'Jetzt lesen'}
                  </button>
                  <button
                    type="button"
                    className="g-btn g-btn--plain"
                    onClick={() => onCameraSettings(c.camId)}
                  >
                    Einstellungen
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {error && <p className="g-error">Nicht angefordert: {error}</p>}
      {requested.size > 0 && (
        <p className="g-hint" role="status">
          Der Hub liest beim nächsten Kontakt
          {next ? ` (gegen ${clockTime(next, now)})` : ''}.
        </p>
      )}
      <div className="g-card__actions">
        <button type="button" className="g-btn" onClick={() => void readAll()}>
          Alle Kameras jetzt lesen
        </button>
        <button type="button" className="g-btn g-btn--plain" onClick={onManageCameras}>
          Kameras verwalten
        </button>
      </div>
    </>
  );
}

const ORIGIN: Record<ScheduleOrigin, string> = {
  'built-in': 'Standard der Firmware',
  local: 'am Hub eingestellt',
  remote: 'aus der App',
  gnss: 'per GNSS ermittelt',
};

function originText(origin: ScheduleOrigin | undefined): string {
  return origin ? ORIGIN[origin] : '';
}

/** The 24-hour bar: windows darker, the quiet hours hatched (#431). */
export function DayBar({
  plan,
  sun,
  quiet,
}: {
  plan: EditPlan;
  sun: SunTimes;
  quiet?: { start: number; end: number };
}) {
  const segments = daySegments(plan, sun);
  const pct = (minute: number) => `${(minute / 1440) * 100}%`;
  const quietParts = quiet
    ? quiet.start < quiet.end
      ? [[quiet.start, quiet.end]]
      : [
          [quiet.start, 1440],
          [0, quiet.end],
        ]
    : [];
  const label = segments
    .map((s) => `${clockText(s.start)}–${clockText(s.end)}: ${everyText(s.intervalMin)}`)
    .join('; ');
  return (
    <div className="g-daybar" role="img" aria-label={`Kamera lesen: ${label}`}>
      <div className="g-daybar__track">
        {segments.map((s) => (
          <span
            key={s.start}
            className={
              s.intervalMin === 0
                ? 'g-daybar__seg g-daybar__seg--off'
                : s.window
                  ? 'g-daybar__seg g-daybar__seg--window'
                  : 'g-daybar__seg'
            }
            style={{ left: pct(s.start), width: pct(s.end - s.start) }}
            title={`${clockText(s.start)}–${clockText(s.end)}: ${everyText(s.intervalMin)}`}
          />
        ))}
        {quietParts.map(([start = 0, end = 0]) => (
          <span
            key={start}
            className="g-daybar__quiet"
            style={{ left: pct(start), width: pct(end - start) }}
            title="Nachtruhe"
          />
        ))}
        <span className="g-daybar__sun" style={{ left: pct(sun.sunrise) }} title="Sonnenaufgang" />
        <span className="g-daybar__sun" style={{ left: pct(sun.sunset) }} title="Sonnenuntergang" />
      </div>
      <div className="g-daybar__scale" aria-hidden="true">
        {[0, 6, 12, 18, 24].map((h) => (
          <span key={h} style={{ left: pct(h * 60) }}>
            {h}
          </span>
        ))}
      </div>
    </div>
  );
}

function planWords(plan: EditPlan): string[] {
  const lines = plan.windows.map(
    (w) => `${boundWords(w.start)} bis ${boundWords(w.end)}: ${everyText(w.intervalMin)}`,
  );
  lines.push(
    `${plan.windows.length > 0 ? 'Sonst' : 'Den ganzen Tag'}: ${everyText(plan.defaultMin)}`,
  );
  return lines;
}

function contactText(minutes: number): string {
  if (minutes === 1440) return 'einmal am Tag';
  return everyText(minutes);
}

/**
 * The schedule editor (#431, #436): presets, the 24-hour bar, the plan as
 * text, quiet hours, location and the contact interval. Empty means the hub's
 * own setting; "Hub-Einstellung verwenden" removes everything set here.
 */
export function ScheduleEditor({
  api,
  hubId,
  config,
  schedule,
  cameraCount,
  nextContact,
  now,
  onSaved,
}: {
  api: GalleryApi;
  hubId: string;
  config: ScheduleConfig;
  schedule?: HubSchedule;
  cameraCount: number;
  nextContact?: number;
  now: number;
  onSaved(): void;
}) {
  const [planText, setPlanText] = useState(config.cameraPlan ?? schedule?.cameraPlan ?? '');
  const [quietOn, setQuietOn] = useState(
    (config.quiet ?? schedule?.quiet ?? 'off') !== 'off' &&
      parseQuietText(config.quiet ?? schedule?.quiet ?? '') !== undefined,
  );
  const initialQuiet = parseQuietText(config.quiet ?? schedule?.quiet ?? '');
  const [quietStart, setQuietStart] = useState(clockText(initialQuiet?.start ?? 23 * 60));
  const [quietEnd, setQuietEnd] = useState(clockText(initialQuiet?.end ?? 5 * 60));
  const [location, setLocation] = useState(config.location ?? '');
  const [contact, setContact] = useState(
    config.checkinIntervalS ? String(config.checkinIntervalS / 60) : '',
  );
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ ok: boolean; text: string }>();
  const [geoBusy, setGeoBusy] = useState(false);

  const parsed = useMemo(() => parseEditPlan(planText), [planText]);
  const sun: SunTimes =
    schedule?.sunriseMin !== undefined && schedule.sunsetMin !== undefined
      ? { sunrise: schedule.sunriseMin, sunset: schedule.sunsetMin }
      : DEFAULT_SUN;
  const quietText = `${quietStart}-${quietEnd}`;
  const quiet = quietOn ? parseQuietText(quietText) : undefined;
  const quietInvalid = quietOn && quiet === undefined;
  const locationValue = location.trim() === '' ? undefined : normalizeLocation(location);
  const locationInvalid = location.trim() !== '' && locationValue === undefined;
  const setFromApp = Object.keys(config).some((k) => k !== 'powerSetting');

  const save = async (next: ScheduleConfig) => {
    setBusy(true);
    try {
      await api.setSchedule(hubId, next);
      setMessage({
        ok: true,
        text: nextContact
          ? `Gespeichert. Der Hub übernimmt es beim nächsten Kontakt (gegen ${clockTime(nextContact, now)}).`
          : 'Gespeichert. Der Hub übernimmt es beim nächsten Kontakt.',
      });
      onSaved();
    } catch (e) {
      setMessage({ ok: false, text: `Nicht gespeichert: ${errorText(e)}` });
    } finally {
      setBusy(false);
    }
  };

  const apply = () => {
    if (!parsed.ok || quietInvalid || locationInvalid) return;
    // The power setting is the admin's (#197); it stays as it is. What the
    // user left as the hub has it stays the hub's own setting.
    const next: ScheduleConfig = {};
    if (config.powerSetting) next.powerSetting = config.powerSetting;
    const planValue = formatEditPlan(parsed.plan);
    if (
      planText.trim() !== '' &&
      (config.cameraPlan !== undefined || planValue !== schedule?.cameraPlan)
    ) {
      next.cameraPlan = planValue;
    }
    const quietValue = quietOn ? quietText : 'off';
    if (config.quiet !== undefined || quietValue !== (schedule?.quiet ?? 'off')) {
      next.quiet = quietValue;
    }
    if (locationValue) next.location = locationValue;
    if (contact !== '') next.checkinIntervalS = Number(contact) * 60;
    void save(next);
  };

  const useHub = () => {
    const next: ScheduleConfig = {};
    if (config.powerSetting) next.powerSetting = config.powerSetting;
    void save(next).then(() => {
      setPlanText(schedule?.cameraPlan ?? '');
      setLocation('');
      setContact('');
    });
  };

  const geolocate = () => {
    if (!navigator.geolocation) {
      setMessage({ ok: false, text: 'Der Browser kann den Standort nicht ermitteln.' });
      return;
    }
    setGeoBusy(true);
    navigator.geolocation.getCurrentPosition(
      (position) => {
        setGeoBusy(false);
        setLocation(
          normalizeLocation(`${position.coords.latitude},${position.coords.longitude}`) ?? '',
        );
      },
      () => {
        setGeoBusy(false);
        setMessage({ ok: false, text: 'Standort nicht ermittelt. Bitte von Hand eintragen.' });
      },
      { maximumAge: 600_000, timeout: 15_000 },
    );
  };

  const reads = parsed.ok ? Math.round(readsPerDay(parsed.plan, sun) * cameraCount) : 0;
  const contactMinutes = contact === '' ? (schedule?.checkinIntervalS ?? 0) / 60 : Number(contact);

  return (
    <form
      className="g-editor"
      onSubmit={(event) => {
        event.preventDefault();
        apply();
      }}
    >
      {schedule && (
        <p className="g-hint">
          Im Moment: Kamera lesen{' '}
          {schedule.cameraPlan === 'off' ? 'aus' : `„${schedule.cameraPlan}“`} (
          {originText(schedule.cameraPlanOrigin)}).
        </p>
      )}

      <fieldset>
        <legend>Kameras lesen</legend>
        <div className="g-presets">
          {PRESETS.map((p) => (
            <button
              key={p.name}
              type="button"
              className={
                parsed.ok && formatEditPlan(parsed.plan) === p.plan
                  ? 'g-btn g-btn--plain g-preset g-preset--active'
                  : 'g-btn g-btn--plain g-preset'
              }
              title={p.hint}
              onClick={() => setPlanText(p.plan)}
            >
              {p.name}
            </button>
          ))}
        </div>
        {parsed.ok ? (
          <>
            <DayBar plan={parsed.plan} sun={sun} quiet={quiet} />
            <ul className="g-editor__words">
              {planWords(parsed.plan).map((line) => (
                <li key={line}>{line}</li>
              ))}
            </ul>
            <p className="g-hint">
              Heute Sonnenaufgang {clockText(sun.sunrise)}, Sonnenuntergang {clockText(sun.sunset)}
              {schedule?.sunriseMin === undefined ? ' (geschätzt)' : ''}. Etwa {reads} Lesungen am
              Tag{cameraCount > 1 ? ` für ${cameraCount} Kameras` : ''}
              {quiet ? ', ohne die Nachtruhe' : ''}.
            </p>
          </>
        ) : (
          <p className="g-error">{parsed.error}</p>
        )}
        <label htmlFor="g-plan-text">Als Text (für Fortgeschrittene)</label>
        <input
          id="g-plan-text"
          type="text"
          value={planText}
          autoComplete="off"
          spellCheck={false}
          aria-invalid={!parsed.ok}
          onChange={(event) => setPlanText(event.target.value)}
        />
      </fieldset>

      <fieldset>
        <legend>Nachtruhe</legend>
        <label className="g-check">
          <input
            type="checkbox"
            checked={quietOn}
            onChange={(event) => setQuietOn(event.target.checked)}
          />{' '}
          Nachts weder lesen noch melden
        </label>
        {quietOn && (
          <div className="g-editor__row">
            <label>
              von{' '}
              <input
                type="time"
                value={quietStart}
                onChange={(event) => setQuietStart(event.target.value)}
              />
            </label>
            <label>
              bis{' '}
              <input
                type="time"
                value={quietEnd}
                onChange={(event) => setQuietEnd(event.target.value)}
              />
            </label>
          </div>
        )}
        {quietInvalid && <p className="g-error">Beginn und Ende der Nachtruhe sind gleich.</p>}
        <p className="g-hint">Der Hub schläft dann durch und spart Strom.</p>
      </fieldset>

      <fieldset>
        <legend>Standort</legend>
        <p className="g-hint">
          Für Sonnenauf- und -untergang. Grob genügt (eine Stelle nach dem Komma, etwa 10 km).
          {schedule?.location
            ? ` Im Moment: ${schedule.location} (${originText(schedule.locationOrigin)}).`
            : schedule?.locationMissing
              ? ' Im Moment ist keiner gesetzt; der Hub nimmt die Mitte Deutschlands.'
              : ''}
        </p>
        <div className="g-editor__row">
          <input
            type="text"
            value={location}
            placeholder={schedule?.location ?? '52.5,13.4'}
            aria-label="Standort (Breite, Länge)"
            aria-invalid={locationInvalid}
            autoComplete="off"
            onChange={(event) => setLocation(event.target.value)}
          />
          <button
            type="button"
            className="g-btn g-btn--plain"
            disabled={geoBusy}
            onClick={geolocate}
          >
            {geoBusy ? 'Ermittle …' : 'Standort dieses Geräts'}
          </button>
        </div>
        {locationInvalid && <p className="g-error">Bitte als „Breite, Länge“, z. B. 52.5, 13.4.</p>}
      </fieldset>

      <fieldset>
        <legend>Kontakt</legend>
        <label htmlFor="g-contact">Der Hub meldet sich</label>
        <select id="g-contact" value={contact} onChange={(event) => setContact(event.target.value)}>
          <option value="">
            wie am Hub eingestellt
            {schedule?.checkinIntervalS ? ` (${contactText(schedule.checkinIntervalS / 60)})` : ''}
          </option>
          {CONTACT_STEPS.map((m) => (
            <option key={m} value={String(m)}>
              {contactText(m)}
            </option>
          ))}
          {contact !== '' && !CONTACT_STEPS.includes(Number(contact)) && (
            <option value={contact}>{contactText(Number(contact))}</option>
          )}
        </select>
        <p className="g-hint">
          Neue Bilder, Löschwünsche und Einstellungen gehen nur beim Kontakt hin und her.
          {contactMinutes > 0 && contactMinutes < 60
            ? ' Häufige Kontakte kosten spürbar Akku und Daten.'
            : ''}{' '}
          Bei schwachem Akku verlängert der Hub die Abstände selbst.
        </p>
      </fieldset>

      {message && (
        <p className={message.ok ? 'g-notice g-notice--ok' : 'g-error'} role="status">
          {message.text}
        </p>
      )}
      <div className="g-card__actions">
        <button
          type="submit"
          className="g-btn"
          disabled={busy || !parsed.ok || quietInvalid || locationInvalid}
        >
          Übernehmen
        </button>
        {setFromApp && (
          <button type="button" className="g-btn g-btn--plain" disabled={busy} onClick={useHub}>
            Hub-Einstellung verwenden
          </button>
        )}
      </div>
    </form>
  );
}

function BatteryTab({
  api,
  hubId,
  schedule,
  now,
}: {
  api: GalleryApi;
  hubId: string;
  schedule?: HubSchedule;
  now: number;
}) {
  const [history, setHistory] = useState<HubHistory>();
  const [usage, setUsage] = useState<HubUsage>();
  const [error, setError] = useState<string>();
  useEffect(() => {
    let live = true;
    Promise.all([api.hubHistory(hubId), api.hubUsage(hubId)])
      .then(([h, u]) => {
        if (!live) return;
        setHistory(h);
        setUsage(u);
      })
      .catch((e: unknown) => live && setError(errorText(e)));
    return () => {
      live = false;
    };
  }, [api, hubId]);
  const quiet = quietHours(schedule?.quiet, schedule?.utcOffsetMin);

  if (error) return <p className="g-error">Nicht geladen: {error}</p>;
  if (!history || !usage) return <p className="g-hint">Lade …</p>;
  const lines = usageLines(monthlyUsage(usage.days, now, usage.budgetMB), now);
  return (
    <>
      <h3>Akku (30 Tage)</h3>
      <div className="g-history__charts">
        <BatteryChart label="Hub" samples={history.hub} now={now} quiet={quiet} />
        {history.cameras.map((c) => (
          <BatteryChart
            key={c.camId}
            label={`Kamera „${c.name}“`}
            samples={c.samples}
            now={now}
            camera
          />
        ))}
      </div>
      <h3>Datenmenge</h3>
      {lines.length === 0 ? (
        <p className="g-hint">Noch keine Werte.</p>
      ) : (
        lines.map((line) => (
          <p key={line} className="g-chart__forecast">
            {line}
          </p>
        ))
      )}
    </>
  );
}

/** What the user can do about a hint (#431). */
export function hintTip(fault: Fault): string | undefined {
  switch (fault.kind) {
    case 'cameraBatteryCritical':
    case 'cameraBatteryLow':
      return 'Akkus der Kamera bald tauschen oder laden.';
    case 'hubBatteryCritical':
    case 'hubBatteryLow':
      return 'Hub-Akku laden; bis dahin meldet er sich seltener.';
    case 'sdFull':
    case 'sdNearlyFull':
      return 'Alte Aufnahmen löschen oder die Karte tauschen.';
    case 'sdMissing':
      return 'Eine SD-Karte in die Kamera stecken.';
    case 'hubSilent':
    case 'hubNeverSeen':
      return 'Strom und Empfang des Hubs prüfen.';
    case 'cameraNotRead':
      return 'Prüfen, ob die Kamera an ist und in Reichweite des Hubs steht.';
    default:
      return undefined;
  }
}

function HintsTab({
  api,
  hubId,
  status,
  now,
}: {
  api: GalleryApi;
  hubId: string;
  status: GalleryStatus;
  now: number;
}) {
  const [hints, setHints] = useState<Fault[]>();
  const [error, setError] = useState<string>();
  useEffect(() => {
    let live = true;
    api
      .hubHints(hubId)
      .then((h) => live && setHints(h))
      .catch((e: unknown) => live && setError(errorText(e)));
    return () => {
      live = false;
    };
  }, [api, hubId]);
  if (error) return <p className="g-error">Nicht geladen: {error}</p>;
  if (!hints) return <p className="g-hint">Lade …</p>;
  if (hints.length === 0) return <p className="g-notice g-notice--ok">Alles in Ordnung.</p>;
  const nameOf = (camId?: string) => status.cameras.find((c) => c.camId === camId)?.name;
  return (
    <ul className="g-hints">
      {hints.map((h) => {
        const tip = hintTip(h);
        return (
          <li
            key={`${h.kind}-${h.camId ?? ''}`}
            className={h.level === 'error' ? 'g-hints__item g-hints__item--error' : 'g-hints__item'}
          >
            <span>{faultText(h, nameOf(h.camId), now)}</span>
            {tip && <span className="g-hint"> {tip}</span>}
          </li>
        );
      })}
    </ul>
  );
}
