import { useEffect, useState } from 'react';
import { clockTime } from './text';
import type { GalleryApi, Recording, ShareLink, ShareOptions } from './types';

interface Props {
  api: Pick<GalleryApi, 'shareRecording' | 'shares' | 'revokeShare'>;
  recording: Recording;
  cameraName: string;
  /** Unix seconds. */
  now: number;
  onClose(): void;
}

const DAYS: ShareOptions['days'][] = [1, 7, 30];

const errorText = (error: unknown) => (error instanceof Error ? error.message : String(error));

/**
 * A link to this recording for people without the app (#444). The page behind
 * it shows only the picture or video; it works for the chosen time and can be
 * taken back here. The camera's name is left out unless the user wants it,
 * because it can give away where the camera hangs.
 */
export function ShareDialog({ api, recording, cameraName, now, onClose }: Props) {
  const hasVideo = recording.state === 'videoReady';
  const [days, setDays] = useState<ShareOptions['days']>(7);
  const [kind, setKind] = useState<ShareOptions['kind']>('thumb');
  const [showCamera, setShowCamera] = useState(false);
  const [link, setLink] = useState<{ url: string; expiresAt: number }>();
  const [active, setActive] = useState<ShareLink[]>([]);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string>();
  const [error, setError] = useState<string>();

  const own = active.filter((l) => l.mediaId === recording.mediaId && l.camId === recording.camId);

  const load = async () => {
    try {
      setActive(await api.shares());
    } catch (e) {
      setError(errorText(e));
    }
  };
  useEffect(() => {
    let current = true;
    api
      .shares()
      .then((list) => {
        if (current) setActive(list);
      })
      .catch((e: unknown) => {
        if (current) setError(errorText(e));
      });
    return () => {
      current = false;
    };
  }, [api]);

  const run = async (work: () => Promise<void>) => {
    setBusy(true);
    setError(undefined);
    setMessage(undefined);
    try {
      await work();
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const create = () =>
    run(async () => {
      const made = await api.shareRecording(recording, { days, kind, showCamera });
      setLink({ url: made.url, expiresAt: made.expiresAt });
      await load();
    });

  const copy = (url: string) =>
    run(async () => {
      await navigator.clipboard.writeText(url);
      setMessage('Link kopiert.');
    });

  const send = (url: string) =>
    run(async () => {
      await navigator.share({ url, title: 'Aufnahme der Wildkamera' });
    });

  const revoke = (id: string) =>
    run(async () => {
      await api.revokeShare(id);
      if (link) setLink(undefined);
      setMessage('Link zurückgezogen.');
      await load();
    });

  return (
    <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-share">
      <div className="g-dialog__box g-share">
        <p id="g-share">Aufnahme teilen</p>
        <p className="g-hint">
          Wer den Link hat, sieht nur diese Aufnahme, ohne Anmeldung. Der Link läuft ab und lässt
          sich hier zurückziehen.
        </p>

        <fieldset className="g-settings__group">
          <legend>Was soll zu sehen sein?</legend>
          <div className="g-settings__row g-settings__row--switch">
            <input
              id="g-share-photo"
              type="radio"
              name="g-share-kind"
              checked={kind === 'thumb'}
              onChange={() => setKind('thumb')}
            />
            <label htmlFor="g-share-photo">Das Bild</label>
          </div>
          <div className="g-settings__row g-settings__row--switch">
            <input
              id="g-share-video"
              type="radio"
              name="g-share-kind"
              checked={kind === 'video'}
              disabled={!hasVideo}
              onChange={() => setKind('video')}
            />
            <label htmlFor="g-share-video">
              Das Video{hasVideo ? '' : ' (erst nach „Video holen“)'}
            </label>
          </div>
        </fieldset>

        <div className="g-settings__row">
          <label htmlFor="g-share-days">Gültig für</label>
          <select
            id="g-share-days"
            value={String(days)}
            disabled={busy}
            onChange={(e) => setDays(Number(e.target.value) as ShareOptions['days'])}
          >
            {DAYS.map((d) => (
              <option key={d} value={String(d)}>
                {d === 1 ? '1 Tag' : `${d} Tage`}
              </option>
            ))}
          </select>
        </div>
        <div className="g-settings__row g-settings__row--switch">
          <input
            id="g-share-camera"
            type="checkbox"
            checked={showCamera}
            disabled={busy}
            onChange={(e) => setShowCamera(e.target.checked)}
          />
          <label htmlFor="g-share-camera">Namen der Kamera zeigen („{cameraName}“)</label>
        </div>

        {link && (
          <div className="g-share__link">
            <label htmlFor="g-share-url">
              Dein Link (gilt bis {clockTime(link.expiresAt, now)})
            </label>
            <input
              id="g-share-url"
              readOnly
              value={link.url}
              onFocus={(e) => e.currentTarget.select()}
            />
            <div className="g-dialog__actions">
              <button
                type="button"
                className="g-btn"
                disabled={busy}
                onClick={() => void copy(link.url)}
              >
                Link kopieren
              </button>
              {typeof navigator !== 'undefined' && 'share' in navigator && (
                <button
                  type="button"
                  className="g-btn g-btn--quiet"
                  disabled={busy}
                  onClick={() => void send(link.url)}
                >
                  Senden …
                </button>
              )}
            </div>
          </div>
        )}

        {message && (
          <p className="g-hint" role="status">
            {message}
          </p>
        )}
        {error && (
          <p className="g-error" role="alert">
            {error}
          </p>
        )}

        {own.length > 0 && (
          <div>
            <h3>Aktive Links zu dieser Aufnahme</h3>
            <ul className="g-share__list">
              {own.map((l) => (
                <li key={l.id}>
                  <span>
                    {l.kind === 'video' ? 'Video' : 'Bild'}, bis {clockTime(l.expiresAt, now)}
                  </span>
                  <button
                    type="button"
                    className="g-link"
                    disabled={busy}
                    onClick={() => void revoke(l.id)}
                  >
                    Zurückziehen
                  </button>
                </li>
              ))}
            </ul>
          </div>
        )}

        <div className="g-dialog__actions">
          <button type="button" className="g-btn g-btn--quiet" onClick={onClose}>
            Schließen
          </button>
          {!link && (
            <button type="button" className="g-btn" disabled={busy} onClick={() => void create()}>
              Link erstellen
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
