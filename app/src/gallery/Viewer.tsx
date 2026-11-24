import { useEffect, useRef } from 'react';
import {
  AlertIcon,
  CheckIcon,
  ClockIcon,
  PlayIcon,
  SaveIcon,
  ShareIcon,
  TrashIcon,
  UndoIcon,
} from './Icons';
import { progressSteps } from './progress';
import { cameraTime } from './text';
import type { Recording } from './types';

interface Props {
  /** The recordings being looked through, in the order the gallery shows them. */
  recordings: Recording[];
  index: number;
  /** Play the video instead of showing the picture. */
  video: boolean;
  cameraName: string;
  saved: boolean;
  nextContact: number | undefined;
  now: number;
  busy: boolean;
  onGo(index: number): void;
  onVideo(on: boolean): void;
  onRequestVideo(): void;
  onSave(): void;
  onShare(): void;
  /** With the empty-trigger detection on (#442): what it thinks, and the user's word on it. */
  emptyVerdict?: 'empty' | 'animal' | 'unsure';
  onCorrectEmpty?(value: 'empty' | 'animal'): void;
  onWithdraw(): void;
  onDelete(): void;
  onClose(): void;
}

const STEP_ICON = { done: CheckIcon, current: ClockIcon, failed: AlertIcon, todo: undefined };

/**
 * A recording large (#306, #431): the picture or its video next to a panel
 * with the actions and how far the recording is. Arrow keys, swipe and the
 * film strip move through the recordings the gallery shows; Esc closes, Space
 * plays, Del asks to delete. The video streams from the backend's storage.
 */
export function Viewer({
  recordings,
  index,
  video,
  cameraName,
  saved,
  nextContact,
  now,
  busy,
  onGo,
  onVideo,
  onRequestVideo,
  onSave,
  onShare,
  emptyVerdict,
  onCorrectEmpty,
  onWithdraw,
  onDelete,
  onClose,
}: Props) {
  const recording = recordings[index];
  const opener = useRef<Element | null>(null);
  const swipe = useRef<number | undefined>(undefined);

  // The focus goes back to the tile that was opened.
  useEffect(() => {
    opener.current = document.activeElement;
    return () => {
      if (opener.current instanceof HTMLElement && opener.current.isConnected) {
        opener.current.focus();
      }
    };
  }, []);

  if (!recording) return null;
  const title = `Aufnahme vom ${cameraTime(recording.mediaTime)}`;
  const url = recording.videoUrl;
  // The demo has no real video (its "videos" are small text files).
  const playable = video && url !== undefined && !url.startsWith('demo:');
  const hasVideo = recording.fileType === 2;
  const go = (to: number) => {
    if (to >= 0 && to < recordings.length) onGo(to);
  };
  const deletable =
    recording.state === 'new' || recording.state === 'failed' || recording.state === 'videoReady';
  const near = recordings.slice(Math.max(0, index - 4), index + 5);
  const firstNear = Math.max(0, index - 4);

  return (
    <div
      className="g-dialog g-viewer"
      role="dialog"
      aria-modal="true"
      aria-label={title}
      onKeyDown={(event) => {
        const onPlayer = event.target instanceof HTMLElement && event.target.tagName === 'VIDEO';
        if (event.key === 'Escape') onClose();
        else if (event.key === 'ArrowLeft') go(index - 1);
        else if (event.key === 'ArrowRight') go(index + 1);
        else if (event.key === 'Delete' && deletable && !busy) onDelete();
        else if (event.key === ' ' && recording.state === 'videoReady' && !onPlayer) {
          event.preventDefault();
          onVideo(!video);
        }
      }}
      onTouchStart={(event) => {
        swipe.current = event.touches[0]?.clientX;
      }}
      onTouchEnd={(event) => {
        const from = swipe.current;
        const to = event.changedTouches[0]?.clientX;
        swipe.current = undefined;
        if (from === undefined || to === undefined || Math.abs(to - from) < 60) return;
        go(to < from ? index + 1 : index - 1);
      }}
    >
      <div className="g-viewer__layout">
        <div className="g-viewer__stage">
          <div className="g-viewer__top">
            <button
              type="button"
              className="g-icon-btn"
              aria-label="Schließen"
              title="Schließen"
              autoFocus
              onClick={onClose}
            >
              ✕
            </button>
            <span>
              {cameraName} · {index + 1} von {recordings.length}
            </span>
            <span className="g-viewer__keys">
              ← → blättern · Entf löschen · Leertaste abspielen
            </span>
          </div>
          {playable ? (
            <video src={url} controls autoPlay playsInline className="g-viewer__media">
              Dieser Browser kann das Video nicht abspielen. Bitte „Video speichern“ nutzen.
            </video>
          ) : (
            <img
              src={recording.thumbUrl}
              alt={`${title}, ${cameraName}`}
              className="g-viewer__media"
            />
          )}
          {video && !playable && (
            <p className="g-viewer__note">In der Vorführung gibt es kein echtes Video.</p>
          )}
          <button
            type="button"
            className="g-viewer__nav g-viewer__nav--prev"
            aria-label="Vorherige Aufnahme"
            disabled={index === 0}
            onClick={() => go(index - 1)}
          >
            ‹
          </button>
          <button
            type="button"
            className="g-viewer__nav g-viewer__nav--next"
            aria-label="Nächste Aufnahme"
            disabled={index === recordings.length - 1}
            onClick={() => go(index + 1)}
          >
            ›
          </button>
          <div className="g-viewer__film">
            {near.map((r, i) => (
              <button
                key={`${r.camId}/${r.mediaId}`}
                type="button"
                className={firstNear + i === index ? 'is-current' : undefined}
                aria-label={`Aufnahme vom ${cameraTime(r.mediaTime)}`}
                aria-current={firstNear + i === index}
                onClick={() => go(firstNear + i)}
              >
                <img src={r.thumbUrl} alt="" />
              </button>
            ))}
          </div>
        </div>

        <aside className="g-viewer__side" aria-label="Einzelheiten">
          <div>
            <h2>{cameraTime(recording.mediaTime)}</h2>
            <p className="g-viewer__sub">
              Kamera {cameraName} · {hasVideo ? 'Foto und Video' : 'Foto'}
            </p>
          </div>
          <div className="g-viewer__actions">
            {(recording.state === 'new' || recording.state === 'failed') && hasVideo && (
              <button type="button" className="g-btn" disabled={busy} onClick={onRequestVideo}>
                Video holen
              </button>
            )}
            {recording.state === 'videoReady' && (
              <>
                <button type="button" className="g-btn" onClick={() => onVideo(!video)}>
                  <PlayIcon /> {video ? 'Foto zeigen' : 'Video abspielen'}
                </button>
                <button type="button" className="g-btn g-btn--quiet" onClick={onSave}>
                  <SaveIcon /> {saved ? 'Nochmal speichern' : 'Speichern'}
                </button>
              </>
            )}
            {recording.state !== 'deleted' && recording.state !== 'deleteRequested' && (
              <button type="button" className="g-btn g-btn--quiet" onClick={onShare}>
                <ShareIcon /> Teilen
              </button>
            )}
            {emptyVerdict && onCorrectEmpty && (
              <button
                type="button"
                className="g-btn g-btn--quiet"
                onClick={() => onCorrectEmpty(emptyVerdict === 'empty' ? 'animal' : 'empty')}
              >
                {emptyVerdict === 'empty' ? 'Doch nicht leer' : 'Als leer markieren'}
              </button>
            )}
            {(recording.state === 'videoRequested' || recording.state === 'deleteRequested') && (
              <button
                type="button"
                className="g-btn g-btn--quiet"
                disabled={busy}
                onClick={onWithdraw}
              >
                <UndoIcon /> Zurücknehmen
              </button>
            )}
            {deletable && (
              <button
                type="button"
                className="g-btn g-btn--quiet g-btn--danger-text"
                disabled={busy}
                onClick={onDelete}
              >
                <TrashIcon /> Auf der Kamera löschen
              </button>
            )}
          </div>
          <div>
            <h3>Fortschritt</h3>
            <ol className="g-steps">
              {progressSteps(recording, saved, nextContact, now).map((step) => {
                const Icon = STEP_ICON[step.status];
                return (
                  <li key={step.text} className={`g-steps__item g-steps__item--${step.status}`}>
                    <span className="g-steps__mark" aria-hidden="true">
                      {Icon && <Icon />}
                    </span>
                    {step.text}
                  </li>
                );
              })}
            </ol>
          </div>
          <details className="g-viewer__details">
            <summary>Einzelheiten</summary>
            <dl>
              <dt>Dateien</dt>
              <dd className="g-viewer__mono">{recording.cameraFiles.join(', ')}</dd>
              <dt>Kamerazeit</dt>
              <dd>{cameraTime(recording.mediaTime)}</dd>
              <dt>Kamera</dt>
              <dd>{cameraName}</dd>
            </dl>
          </details>
        </aside>
      </div>
    </div>
  );
}
