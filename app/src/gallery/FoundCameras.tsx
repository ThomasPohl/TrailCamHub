import { useState } from 'react';
import { clockTime } from './text';
import { MAX_CAMERA_NAME, type FoundCamera } from './types';

/**
 * Cameras the hub heard but does not read (#298): new ones to take in with a
 * name or to ignore, and ignored ones to take in after all. Every user
 * linked to the hub may decide (decision 16 Nov 2026).
 */

export function signalText(rssi: number | undefined): string {
  if (rssi === undefined) return '';
  if (rssi >= -65) return 'starkes Signal';
  if (rssi >= -80) return 'mittleres Signal';
  return 'schwaches Signal';
}

export function FoundCameras({
  found,
  busy,
  now,
  onDecide,
}: {
  found: FoundCamera[];
  busy: boolean;
  now: number;
  onDecide(camera: FoundCamera, state: 'approved' | 'excluded', name?: string): void;
}) {
  const fresh = found.filter((f) => f.state === 'new');
  const ignored = found.filter((f) => f.state === 'excluded');
  if (fresh.length === 0 && ignored.length === 0) return null;
  return (
    <section className="g-found" aria-label="Gefundene Kameras">
      {fresh.length > 0 && (
        <>
          <h2>Neue Kameras gefunden</h2>
          <p className="g-hint">
            Der Hub hat diese Kameras in der Nähe gehört. Er liest sie erst, wenn Sie sie aufnehmen.
          </p>
          <div>
            {fresh.map((f) => (
              <NewCamera
                key={`${f.hubId}/${f.bleAddress}`}
                camera={f}
                busy={busy}
                now={now}
                onDecide={onDecide}
              />
            ))}
          </div>
        </>
      )}
      {ignored.length > 0 && (
        <>
          <h2>Nicht abgefragte Kameras</h2>
          <div>
            {ignored.map((f) => (
              <div key={`${f.hubId}/${f.bleAddress}`} className="g-found__item">
                <span>{f.name}</span>
                <button
                  type="button"
                  className="g-btn g-btn--plain"
                  disabled={busy}
                  onClick={() => onDecide(f, 'approved')}
                >
                  Doch aufnehmen
                </button>
              </div>
            ))}
          </div>
        </>
      )}
    </section>
  );
}

function NewCamera({
  camera,
  busy,
  now,
  onDecide,
}: {
  camera: FoundCamera;
  busy: boolean;
  now: number;
  onDecide(camera: FoundCamera, state: 'approved' | 'excluded', name?: string): void;
}) {
  const [name, setName] = useState('');
  const id = `g-found-${camera.bleAddress.replace(/:/g, '')}`;
  const details = [
    signalText(camera.rssi),
    camera.lastSeen > 0 ? `zuletzt gehört ${clockTime(camera.lastSeen, now)}` : '',
  ].filter(Boolean);
  return (
    // Not a list item: the recordings are the page's list.
    <div
      role="group"
      className="g-found__item g-found__item--new"
      aria-label={`Neue Kamera ${camera.name}`}
    >
      <p>
        <strong>{camera.name}</strong>
        {details.length > 0 && <span className="g-hint"> · {details.join(' · ')}</span>}
      </p>
      <form
        className="g-rename"
        onSubmit={(event) => {
          event.preventDefault();
          onDecide(camera, 'approved', name.trim() || undefined);
        }}
      >
        <label htmlFor={id}>Name der Kamera (z. B. „Futterplatz“)</label>
        <input
          id={id}
          type="text"
          value={name}
          maxLength={MAX_CAMERA_NAME}
          autoComplete="off"
          placeholder={camera.name}
          onChange={(event) => setName(event.target.value)}
        />
        <button type="submit" className="g-btn" disabled={busy}>
          Aufnehmen
        </button>
        <button
          type="button"
          className="g-btn g-btn--plain"
          disabled={busy}
          onClick={() => onDecide(camera, 'excluded')}
        >
          Ignorieren
        </button>
      </form>
    </div>
  );
}
