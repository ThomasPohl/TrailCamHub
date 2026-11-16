import { useCallback, useEffect, useState } from 'react';
import type { Camera, ScheduleConfig, ScheduleOrigin, ScheduleStretch } from '../api/types';
import { clockTime } from './text';
import type { AdminHub, GalleryApi } from './types';

/**
 * The admin view (#264): every hub with its state, cameras and users, the
 * schedule and a camera read on request. For the maintainer, in the same
 * app as the gallery; users and hub links stay with `npm run add-user`.
 */

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

export function AdminView({ api, now }: { api: GalleryApi; now: () => number }) {
  const [hubs, setHubs] = useState<AdminHub[]>();
  const [error, setError] = useState<string>();
  const [notice, setNotice] = useState<string>();
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    try {
      setHubs(await api.adminHubs());
      setError(undefined);
    } catch (e) {
      setError(errorText(e));
    }
  }, [api]);

  useEffect(() => {
    void load();
  }, [load]);

  const act = async (run: () => Promise<void>, success: string) => {
    setBusy(true);
    try {
      await run();
      setNotice(success);
      await load();
    } catch (e) {
      setNotice(`Das hat nicht geklappt: ${errorText(e)}`);
    } finally {
      setBusy(false);
    }
  };

  const t = now();

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
      {hubs?.map((hub) => (
        <article key={hub.hubId} className="g-hub" aria-label={`Hub ${hub.name}`}>
          <h2>
            Hub „{hub.name}“{hub.name !== hub.hubId && <small> ({hub.hubId})</small>}
          </h2>
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
            <dt>Firmware</dt>
            <dd>{hub.firmware ?? '–'}</dd>
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
              {hub.users.length === 0
                ? 'keine'
                : hub.users
                    .map((u) =>
                      u.name
                        ? `${u.name}${u.role === 'admin' ? ' (Admin)' : ''}`
                        : `Telegram ${u.userId}`,
                    )
                    .join(', ')}
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

          <ScheduleForm
            key={`${hub.hubId}:${hub.scheduleConfig.cameraPlan ?? ''}:${hub.scheduleConfig.checkinIntervalS ?? ''}`}
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
    </section>
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
  const minutesValue = interval.trim() === '' ? undefined : Number(interval);
  const invalid =
    minutesValue !== undefined &&
    !(Number.isInteger(minutesValue) && minutesValue >= 5 && minutesValue <= 1440);
  const planId = `g-plan-${hub.hubId}`;
  const intervalId = `g-interval-${hub.hubId}`;

  return (
    <form
      className="g-schedule"
      onSubmit={(event) => {
        event.preventDefault();
        if (invalid) return;
        const config: ScheduleConfig = {};
        if (plan.trim()) config.cameraPlan = plan.trim();
        if (minutesValue !== undefined) config.checkinIntervalS = minutesValue * 60;
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
      <div className="g-card__actions">
        <button type="submit" className="g-btn" disabled={disabled || invalid}>
          Übernehmen
        </button>
      </div>
    </form>
  );
}
