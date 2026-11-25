import {
  candidatesOf,
  METHOD,
  measure,
  neighboursOf,
  seriesOf,
  verdictFor,
  type Frame,
  type Sensitivity,
  type Timeline,
  type Verdict,
} from './emptiness';
import { recordingKey } from './grouping';
import type { Recording } from './types';

/**
 * Finds the likely empty triggers of a camera's recordings (#442). The score
 * of each picture is kept on this device (`EmptyStore`); what counts as empty
 * is decided when it is shown, from the score and the chosen sensitivity. A
 * correction by the user always wins and is never overwritten.
 */

export interface Detected {
  /** Share of the picture that the largest moving patch covers. */
  score: number;
  /** No judgement was possible when it was made. */
  unsure: boolean;
  /**
   * Why it is unsure: too few neighbours (`few`, tried again when the camera
   * has more pictures), the whole scene changed (`scene`, final) or the
   * picture could not be read (`unreadable`, tried again after a while).
   */
  reason?: 'few' | 'scene' | 'unreadable';
  /** With `few`: how many pictures the camera had then. */
  pool?: number;
  method: string;
  /** Unix seconds. */
  at: number;
  /** The user's word: it is empty, or it is not. */
  corrected?: 'empty' | 'animal';
}

export interface EmptyStore {
  all(): Promise<Map<string, Detected>>;
  put(key: string, entry: Detected): Promise<void>;
}

export function memoryEmptyStore(): EmptyStore {
  const map = new Map<string, Detected>();
  return {
    all: async () => new Map(map),
    put: async (key, entry) => {
      map.set(key, { ...entry });
    },
  };
}

const DB = 'trailcamhub-gallery';
const STORE = 'empty-triggers';

const done = <T>(request: IDBRequest<T>) =>
  new Promise<T>((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB'));
  });

/** IndexedDB; without it (private mode, old browser) the results live until the page closes. */
export function openEmptyStore(): EmptyStore {
  if (typeof indexedDB === 'undefined') return memoryEmptyStore();
  const opened = new Promise<IDBDatabase>((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB'));
  });
  // A failed open is handled where the database is used; it must not be reported as unhandled.
  opened.catch(() => undefined);
  const fallback = memoryEmptyStore();
  return {
    async all() {
      try {
        const db = await opened;
        // Keys and values from one transaction, so they belong together.
        const store = db.transaction(STORE, 'readonly').objectStore(STORE);
        const [keys, values] = await Promise.all([done(store.getAllKeys()), done(store.getAll())]);
        const map = new Map(keys.map((k, i) => [String(k), values[i] as Detected]));
        for (const [key, entry] of await fallback.all()) map.set(key, entry);
        return map;
      } catch {
        return fallback.all();
      }
    },
    async put(key, entry) {
      try {
        const db = await opened;
        await done(db.transaction(STORE, 'readwrite').objectStore(STORE).put(entry, key));
      } catch {
        await fallback.put(key, entry);
      }
    },
  };
}

/** What a recording counts as: the user's word first, else the score by sensitivity. */
export function verdictOf(entry: Detected | undefined, sensitivity: Sensitivity): Verdict {
  if (!entry) return 'unsure';
  if (entry.corrected) return entry.corrected;
  return verdictFor({ score: entry.score, unsure: entry.unsure }, sensitivity);
}

/** A picture that could not be read is tried again after this many seconds. */
export const RETRY_UNREADABLE_S = 60 * 60;

/** Whether a recording needs (another) analysis. */
export function needsAnalysis(entry: Detected | undefined, pool: number, now: number): boolean {
  if (!entry) return true;
  if (entry.corrected) return false;
  if (entry.method !== METHOD) return true;
  if (!entry.unsure) return false;
  switch (entry.reason) {
    case 'scene':
      return false;
    case 'unreadable':
      return now - entry.at >= RETRY_UNREADABLE_S;
    default:
      // `few`, or an entry from before the reason was kept: when more pictures exist.
      return entry.pool === undefined || pool > entry.pool;
  }
}

export interface Analysis {
  /** Entries made or changed in this run, by recording key. */
  results: Map<string, Detected>;
}

export interface AnalyseOptions {
  /** The picture as a frame; `undefined` if it cannot be read. */
  loadFrame(url: string): Promise<Frame | undefined>;
  known: ReadonlyMap<string, Detected>;
  now: number;
  /**
   * Frames read before, by recording key; new ones are added. A run that is
   * cancelled (a new recording came in) thus does not read them again.
   */
  frames?: Map<string, Frame>;
  /** Pictures read at the same time. */
  concurrency?: number;
  onProgress?(done: number, total: number): void;
  isCancelled?(): boolean;
}

/**
 * Analyses the recordings of one camera that have no usable result yet (see
 * `needsAnalysis`). Their neighbours are read as well; they are only compared
 * with, not stored again.
 */
export async function analyseCamera(
  recordings: readonly Recording[],
  options: AnalyseOptions,
): Promise<Analysis> {
  const sorted = [...recordings].sort((a, b) => a.mediaTime - b.mediaTime || a.mediaId - b.mediaId);
  const times = sorted.map((r) => r.mediaTime);
  const timeline: Timeline = { times, series: seriesOf(times) };
  const todo: number[] = [];
  sorted.forEach((r, i) => {
    if (needsAnalysis(options.known.get(recordingKey(r)), sorted.length, options.now)) todo.push(i);
  });
  const results = new Map<string, Detected>();
  if (todo.length === 0) return { results };

  // The pictures to read: those to judge and their possible neighbours.
  const needed = new Set<number>();
  for (const i of todo) {
    needed.add(i);
    for (const n of candidatesOf(sorted.length, i, timeline)) needed.add(n);
  }
  const cache = options.frames ?? new Map<string, Frame>();
  const frames: (Frame | undefined)[] = new Array<Frame | undefined>(sorted.length);
  const queue = [...needed].sort((a, b) => a - b);
  let count = 0;
  const worker = async () => {
    for (let i = queue.shift(); i !== undefined; i = queue.shift()) {
      if (options.isCancelled?.()) return;
      const recording = sorted[i] as Recording;
      const key = recordingKey(recording);
      let frame = cache.get(key);
      if (!frame) {
        frame = await options.loadFrame(recording.thumbUrl);
        if (frame) cache.set(key, frame);
      }
      frames[i] = frame;
      count += 1;
      options.onProgress?.(count, needed.size);
    }
  };
  await Promise.all(Array.from({ length: options.concurrency ?? 4 }, worker));
  if (options.isCancelled?.()) return { results };

  for (const i of todo) {
    const key = recordingKey(sorted[i] as Recording);
    const frame = frames[i];
    if (!frame) {
      results.set(key, {
        score: 0,
        unsure: true,
        reason: 'unreadable',
        method: METHOD,
        at: options.now,
      });
      continue;
    }
    const result = measure(frame, neighboursOf(frames, i, timeline));
    results.set(key, {
      score: result.score,
      unsure: result.unsure,
      ...(result.reason && { reason: result.reason }),
      ...(result.reason === 'few' && { pool: sorted.length }),
      method: METHOD,
      at: options.now,
    });
  }
  return { results };
}
