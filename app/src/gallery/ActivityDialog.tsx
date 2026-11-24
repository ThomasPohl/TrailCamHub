import { useEffect, useMemo, useState, type CSSProperties } from 'react';
import { activityEvents, eventsPerDay, heatmap, suggestPlan, summaryText } from './activity';
import type { GalleryApi, GalleryStatus } from './types';

interface Props {
  api: Pick<GalleryApi, 'recordings' | 'setSchedule'>;
  status: GalleryStatus;
  /** Unix seconds; the camera clocks are compared with it, so a camera clock off by hours is shown as it is. */
  now: number;
  onClose(): void;
}

const WEEKDAYS = ['Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa', 'So'];
const HOURS = Array.from({ length: 24 }, (_, h) => h);
const DAYS_SHOWN = 90;

const errorText = (error: unknown) => (error instanceof Error ? error.message : String(error));

/**
 * When do the cameras record? (#443) A heatmap of weekday and hour, the
 * events of the last 90 days, and a suggestion for the hub's camera read plan
 * from the hours with the most events. Built from the recordings the gallery
 * loads anyway; nothing new is stored. Recordings within a minute count once,
 * and a camera whose clock was never set is left out.
 */
export function ActivityDialog({ api, status, now, onClose }: Props) {
  const cameras = useMemo(() => status.cameras.filter((c) => !c.hidden), [status.cameras]);
  const [times, setTimes] = useState<Map<string, number[]>>();
  const [camId, setCamId] = useState('');
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string>();

  useEffect(() => {
    let current = true;
    Promise.all(
      cameras.map(async (c) => {
        const recordings = await api.recordings(c.camId);
        return [c.camId, recordings.map((r) => r.mediaTime)] as const;
      }),
    )
      .then((rows) => {
        if (current) setTimes(new Map(rows));
      })
      .catch((e: unknown) => {
        if (current) setError(errorText(e));
      });
    return () => {
      current = false;
    };
  }, [api, cameras]);

  const view = useMemo(() => {
    if (!times) return undefined;
    const pick = (id: string) =>
      (id === '' ? [...times.values()].flat() : (times.get(id) ?? [])).map((mediaTime) => ({
        mediaTime,
      }));
    const shown = activityEvents(pick(camId));
    const all = camId === '' ? shown : activityEvents(pick(''));
    const map = heatmap(shown.events);
    const last = shown.events[shown.events.length - 1] ?? now;
    return {
      ...shown,
      map,
      perDay: eventsPerDay(shown.events, Math.max(last, now), DAYS_SHOWN),
      summary: summaryText(map.byHour),
      suggestion: suggestPlan(all.events),
    };
  }, [times, camId, now]);

  const hub = status.hub;
  const apply = async (plan: string) => {
    if (!hub) return;
    setBusy(true);
    setError(undefined);
    try {
      // The other settings stay as they are.
      await api.setSchedule(hub.hubId, { ...hub.config, cameraPlan: plan });
      setMessage('Gespeichert. Der Hub übernimmt den Plan beim nächsten Kontakt.');
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const peak = Math.max(1, ...(view?.perDay.map((d) => d.count) ?? [1]));
  const activeDays = view?.perDay.filter((d) => d.count > 0).length ?? 0;

  return (
    <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-activity">
      <div className="g-dialog__box g-activity">
        <p id="g-activity">Wann sind die Kameras aktiv?</p>
        <div className="g-settings__row">
          <label htmlFor="g-activity-cam">Kamera</label>
          <select id="g-activity-cam" value={camId} onChange={(e) => setCamId(e.target.value)}>
            <option value="">Alle Kameras</option>
            {cameras.map((c) => (
              <option key={c.camId} value={c.camId}>
                {c.name}
              </option>
            ))}
          </select>
        </div>

        {error && (
          <p className="g-error" role="alert">
            {error}
          </p>
        )}
        {!view && !error && <p className="g-hint">Lade …</p>}

        {view && view.events.length === 0 && (
          <p className="g-hint">Es gibt noch keine Aufnahmen, die sich auswerten lassen.</p>
        )}

        {view && view.events.length > 0 && (
          <>
            <p role="status">
              {view.events.length} Ereignisse
              {view.summary ? `: ${view.summary}.` : '.'}
            </p>
            {view.ignored > 0 && (
              <p className="g-hint">
                {view.ignored} Aufnahmen fehlen, weil die Uhr der Kamera nicht gestellt war.
              </p>
            )}

            <div className="g-activity__scroll">
              <table className="g-activity__heat">
                <caption className="g-sr">Ereignisse je Wochentag und Stunde der Kamerauhr</caption>
                <thead>
                  <tr>
                    <td />
                    {HOURS.map((h) => (
                      <th key={h} scope="col">
                        {h}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {view.map.cells.map((row, d) => (
                    <tr key={WEEKDAYS[d]}>
                      <th scope="row">{WEEKDAYS[d]}</th>
                      {row.map((count, h) => (
                        <td
                          key={h}
                          className={
                            count > 0 ? 'g-activity__cell g-activity__cell--on' : 'g-activity__cell'
                          }
                          style={{ '--g-heat': count / view.map.max } as CSSProperties}
                        >
                          {count > 0 ? count : ''}
                        </td>
                      ))}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            <h3>Letzte {DAYS_SHOWN} Tage</h3>
            <div
              className="g-activity__bars"
              role="img"
              aria-label={`Ereignisse pro Tag in den letzten ${DAYS_SHOWN} Tagen: an ${activeDays} Tagen gab es welche, am meisten ${peak} an einem Tag`}
            >
              {view.perDay.map((d) => (
                <span
                  key={d.day}
                  title={`${d.day}: ${d.count}`}
                  style={{ height: `${Math.max(d.count > 0 ? 6 : 1, (d.count / peak) * 100)}%` }}
                />
              ))}
            </div>
          </>
        )}

        {view && (
          <section>
            <h3>Vorschlag für den Leseplan</h3>
            {view.suggestion.ok ? (
              <>
                <p>
                  <code>{view.suggestion.plan}</code>
                  <br />
                  Alle 30 Minuten in {view.suggestion.windows.join(' und ')}, sonst alle 2 Stunden.
                  Die Fenster decken {Math.round(view.suggestion.coverage * 100)} % der Ereignisse
                  aller Kameras ab.
                </p>
                <p className="g-hint">
                  Jetzt am Hub: <code>{hub?.cameraPlan ?? 'unbekannt'}</code>. Mehr Lesungen kosten
                  Akku der Kamera.
                </p>
                {hub && (
                  <button
                    type="button"
                    className="g-btn"
                    disabled={busy || hub.cameraPlan === view.suggestion.plan}
                    onClick={() => void apply(view.suggestion.ok ? view.suggestion.plan : '')}
                  >
                    Plan übernehmen
                  </button>
                )}
              </>
            ) : (
              <p className="g-hint">{view.suggestion.reason}</p>
            )}
            {message && (
              <p className="g-hint" role="status">
                {message}
              </p>
            )}
          </section>
        )}

        <div className="g-dialog__actions">
          <button type="button" className="g-btn g-btn--quiet" onClick={onClose}>
            Schließen
          </button>
        </div>
      </div>
    </div>
  );
}
