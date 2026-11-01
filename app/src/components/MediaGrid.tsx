import type { MediaItem } from '../api/types';
import { formatCameraTime, mediaKey } from '../format';

interface Props {
  items: MediaItem[];
  onSelect(item: MediaItem): void;
}

export function MediaGrid({ items, onSelect }: Props) {
  if (items.length === 0) {
    return <p className="hint empty">No recordings yet.</p>;
  }
  return (
    <ul className="grid" aria-label="Recordings">
      {items.map((item) => (
        <li key={mediaKey(item)}>
          <button type="button" className="tile" onClick={() => onSelect(item)}>
            <img src={item.thumbUrl} alt={`Recording ${mediaKey(item)}`} loading="lazy" />
            <span className="tile__meta">
              {item.fileType === 2 && (
                <span className="badge" aria-label="Video">
                  ▶
                </span>
              )}
              {formatCameraTime(item.mediaTime)}
            </span>
          </button>
        </li>
      ))}
    </ul>
  );
}
