import type { MediaItem } from '../api/types';
import { canRequestVideo, formatCameraTime, mediaKey, videoStateLabel } from '../format';

interface Props {
  item: MediaItem;
  busy: boolean;
  showBackLink: boolean;
  onBack(): void;
  onRequestVideo(item: MediaItem): void;
}

export function MediaDetail({ item, busy, showBackLink, onBack, onRequestVideo }: Props) {
  const isVideo = item.fileType === 2;
  return (
    <section className="detail" aria-label={`Recording ${mediaKey(item)}`}>
      {showBackLink && (
        <button type="button" className="link" onClick={onBack}>
          ← All recordings
        </button>
      )}
      <img className="detail__image" src={item.thumbUrl} alt={`Recording ${mediaKey(item)}`} />
      <div className="card">
        <dl className="facts">
          <dt>Recorded</dt>
          <dd>{formatCameraTime(item.mediaTime)}</dd>
          <dt>Type</dt>
          <dd>{isVideo ? 'Video' : 'Photo'}</dd>
          <dt>File</dt>
          <dd>{mediaKey(item)}</dd>
          {isVideo && (
            <>
              <dt>Video</dt>
              <dd>{videoStateLabel(item.videoState)}</dd>
            </>
          )}
        </dl>
        {isVideo && canRequestVideo(item) && (
          <button
            type="button"
            className="primary"
            disabled={busy}
            onClick={() => onRequestVideo(item)}
          >
            {busy ? 'Requesting…' : 'Download video'}
          </button>
        )}
      </div>
    </section>
  );
}
