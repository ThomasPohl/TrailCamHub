import { toCameraWallClock } from '../format';
import type { Recording, RecordingState } from './types';

/** The state filters of the gallery (#431): what the user wants to see. */
export type StateFilter = 'all' | 'new' | 'waiting' | 'ready' | 'failed';

export const STATE_FILTERS: Array<{ id: StateFilter; label: string }> = [
  { id: 'all', label: 'Alle' },
  { id: 'new', label: 'Neu' },
  { id: 'waiting', label: 'Wartet' },
  { id: 'ready', label: 'Video da' },
  { id: 'failed', label: 'Fehler' },
];

/** The filter a state belongs to; "waiting" covers both pending requests. */
export function filterOf(state: RecordingState): Exclude<StateFilter, 'all'> | undefined {
  switch (state) {
    case 'new':
      return 'new';
    case 'videoRequested':
    case 'deleteRequested':
      return 'waiting';
    case 'videoReady':
      return 'ready';
    case 'failed':
      return 'failed';
    case 'deleted':
      return undefined;
  }
}

export function countByFilter(items: Recording[]): Record<StateFilter, number> {
  const counts: Record<StateFilter, number> = { all: 0, new: 0, waiting: 0, ready: 0, failed: 0 };
  for (const r of items) {
    const f = filterOf(r.state);
    if (!f) continue;
    counts.all += 1;
    counts[f] += 1;
  }
  return counts;
}

export function applyFilter(items: Recording[], filter: StateFilter): Recording[] {
  return filter === 'all' ? items : items.filter((r) => filterOf(r.state) === filter);
}

/** Newest first, with the camera and the number as tie-breakers. */
export function newestFirst(items: Recording[]): Recording[] {
  return [...items].sort(
    (a, b) => b.mediaTime - a.mediaTime || a.camId.localeCompare(b.camId) || b.mediaId - a.mediaId,
  );
}

const DAY_S = 86_400;
const WEEKDAYS = ['Sonntag', 'Montag', 'Dienstag', 'Mittwoch', 'Donnerstag', 'Freitag', 'Samstag'];
const MONTHS = [
  'Januar',
  'Februar',
  'März',
  'April',
  'Mai',
  'Juni',
  'Juli',
  'August',
  'September',
  'Oktober',
  'November',
  'Dezember',
];

/** Day number of a camera wall-clock time (the clock is read as UTC). */
const dayOf = (cameraSeconds: number) => Math.floor(cameraSeconds / DAY_S);

/** "Freitag, 20. November" from a day number. */
function longDay(day: number): string {
  const d = new Date(day * DAY_S * 1000);
  return `${WEEKDAYS[d.getUTCDay()]}, ${d.getUTCDate()}. ${MONTHS[d.getUTCMonth()]}`;
}

export interface DayGroup {
  /** Day number, the key of the group. */
  day: number;
  /** "Heute", "Gestern" or "Freitag, 20. November". */
  title: string;
  /** "Montag, 23. November" for today and yesterday, else empty. */
  subtitle: string;
  items: Recording[];
}

/** Groups newest-first recordings by day; `now` is the real instant (Unix s). */
export function groupByDay(items: Recording[], now: number): DayGroup[] {
  const today = dayOf(toCameraWallClock(now));
  const groups: DayGroup[] = [];
  for (const r of items) {
    const day = dayOf(r.mediaTime);
    const last = groups[groups.length - 1];
    if (last && last.day === day) {
      last.items.push(r);
      continue;
    }
    const relative = day === today ? 'Heute' : day === today - 1 ? 'Gestern' : undefined;
    groups.push({
      day,
      title: relative ?? longDay(day),
      subtitle: relative ? longDay(day) : '',
      items: [r],
    });
  }
  return groups;
}

/** The key of a recording across cameras (mediaIds only count per camera). */
export const recordingKey = (r: Pick<Recording, 'camId' | 'mediaId'>) => `${r.camId}/${r.mediaId}`;
