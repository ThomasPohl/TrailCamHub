import { cameraTime } from './text';
import type { Recording } from './types';

/**
 * A recording large (#306): the picture over the whole window, or its video
 * with the browser's player. The video streams from the backend's storage;
 * saving it into the folder stays a separate button.
 */
export function Viewer({
  recording,
  video,
  onClose,
}: {
  recording: Recording;
  /** Play the video instead of showing the picture. */
  video: boolean;
  onClose(): void;
}) {
  const title = `Aufnahme vom ${cameraTime(recording.mediaTime)}`;
  const url = recording.videoUrl;
  // The demo has no real video (its "videos" are small text files).
  const playable = video && url !== undefined && !url.startsWith('demo:');
  return (
    <div
      className="g-dialog g-viewer"
      role="dialog"
      aria-modal="true"
      aria-label={title}
      onKeyDown={(event) => {
        if (event.key === 'Escape') onClose();
      }}
    >
      <div className="g-viewer__box">
        <p className="g-viewer__title">{title}</p>
        {playable ? (
          <video src={url} controls autoPlay playsInline className="g-viewer__media">
            Dieser Browser kann das Video nicht abspielen. Bitte „Video speichern“ nutzen.
          </video>
        ) : (
          <img
            src={recording.thumbUrl}
            alt={`${title}, ${recording.cameraFiles.join(', ')}`}
            className="g-viewer__media"
          />
        )}
        {video && !playable && (
          <p className="g-hint">In der Vorführung gibt es kein echtes Video.</p>
        )}
        <div className="g-card__actions">
          <button type="button" className="g-btn" autoFocus onClick={onClose}>
            Schließen
          </button>
        </div>
      </div>
    </div>
  );
}
