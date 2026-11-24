import {
  METHOD,
  NEIGHBOUR_REACH,
  measure,
  neighboursOf,
  verdictFor,
  type Frame,
  type Sensitivity,
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
  /** No judgement was possible when it was made (too few neighbours, scene change). */
  unsure: boolean;
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

/** IndexedDB; without it (private mode, old browser) the results live until the page closes. */
export function openEmptyStore(): EmptyStore {
  if (typeof indexedDB === 'undefined') return memoryEmptyStore();
  const opened = new Promise<IDBDatabase>((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB'));
  });
  const run = async <T>(mode: IDBTransactionMode, work: (s: IDBObjectStore) => IDBRequest<T>) => {
    const db = await opened;
    return new Promise<T>((resolve, reject) => {
      const request = work(db.transaction(STORE, mode).objectStore(STORE));
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error ?? new Error('IndexedDB'));
    });
  };
  const fallback = memoryEmptyStore();
  return {
    async all() {
      try {
        const keys = await run('readonly', (s) => s.getAllKeys());
        const values = await run('readonly', (s) => s.getAll());
        return new Map(keys.map((k, i) => [String(k), values[i] as Detected]));
      } catch {
        return fallback.all();
      }
    },
    async put(key, entry) {
      try {
        await run('readwrite', (s) => s.put(entry, key));
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
  return verdictFor(entry, sensitivity);
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
  /** Pictures read at the same time. */
  concurrency?: number;
  onProgress?(done: number, total: number): void;
  isCancelled?(): boolean;
}

/**
 * Analyses the recordings of one camera that have no usable result yet: new
 * ones, and ones that were unsure because too few neighbours existed then.
 * The neighbours of those are read as well (they are only compared with, not
 * stored again).
 */
export async function analyseCamera(
  recordings: readonly Recording[],
  options: AnalyseOptions,
): Promise<Analysis> {
  const sorted = [...recordings].sort((a, b) => a.mediaTime - b.mediaTime || a.mediaId - b.mediaId);
  const todo = new Set<number>();
  sorted.forEach((r, i) => {
    const entry = options.known.get(recordingKey(r));
    if (!entry || (entry.unsure && !entry.corrected)) todo.add(i);
  });
  const results = new Map<string, Detected>();
  if (todo.size === 0) return { results };

  // The pictures to read: those to judge and their neighbours in time.
  const needed = new Set<number>();
  for (const i of todo) {
    for (let d = -NEIGHBOUR_REACH; d <= NEIGHBOUR_REACH; d++) {
      if (sorted[i + d]) needed.add(i + d);
    }
  }
  const frames = new Map<number, Frame>();
  const queue = [...needed];
  let done = 0;
  const worker = async () => {
    for (let i = queue.shift(); i !== undefined; i = queue.shift()) {
      if (options.isCancelled?.()) return;
      const frame = await options.loadFrame((sorted[i] as Recording).thumbUrl);
      if (frame) frames.set(i, frame);
      done += 1;
      options.onProgress?.(done, needed.size);
    }
  };
  await Promise.all(Array.from({ length: options.concurrency ?? 4 }, worker));
  if (options.isCancelled?.()) return { results };

  // Frames by position, with holes closed up so neighbours are neighbours in time.
  const present = [...frames.keys()].sort((a, b) => a - b);
  const list = present.map((i) => frames.get(i) as Frame);
  for (const i of todo) {
    const at = present.indexOf(i);
    if (at < 0) continue;
    const neighbours = neighboursOf(list, at);
    const result = measure(list[at] as Frame, neighbours);
    // With too few neighbours it stays open for a later run.
    results.set(recordingKey(sorted[i] as Recording), {
      score: result.score,
      unsure: result.unsure,
      method: METHOD,
      at: options.now,
    });
  }
  return { results };
}
