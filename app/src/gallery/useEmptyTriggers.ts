import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  analyseCamera,
  staleKeys,
  verdictOf,
  type Detected,
  type EmptyStore,
} from './emptyDetector';
import { METHOD, type Frame, type Sensitivity, type Verdict } from './emptiness';
import { recordingKey } from './grouping';
import type { Recording } from './types';

export interface EmptyDeps {
  loadFrame(url: string): Promise<Frame | undefined>;
  store: EmptyStore;
}

export interface EmptyProgress {
  done: number;
  total: number;
  /** Which camera is being checked (1-based), and of how many. */
  camera: number;
  cameras: number;
}

const ON_KEY = 'trailcamhub.empty.on';
const SENSITIVITY_KEY = 'trailcamhub.empty.sensitivity';

function read(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function write(key: string, value: string) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // Not saved; it holds until the page closes.
  }
}

/** The recordings' likely empty triggers, found on this device (#442). Off until the user turns it on. */
export function useEmptyTriggers(
  items: Recording[] | undefined,
  deps: EmptyDeps,
  now: () => number,
) {
  const [enabled, setEnabledState] = useState(() => read(ON_KEY) === 'on');
  const [sensitivity, setSensitivityState] = useState<Sensitivity>(() => {
    const saved = read(SENSITIVITY_KEY);
    return saved === 'normal' || saved === 'strict' ? saved : 'careful';
  });
  const [entries, setEntries] = useState<ReadonlyMap<string, Detected>>(new Map());
  const [loaded, setLoaded] = useState(false);
  const [progress, setProgress] = useState<EmptyProgress>();
  // The current entries, also between a change and the next render: the
  // analysis and a correction both write here first, then to the state.
  const entriesRef = useRef<ReadonlyMap<string, Detected>>(entries);
  // Frames read so far, by recording key: a run that is cancelled because a
  // new recording came in does not read them again (the URLs change at every
  // poll, the recordings do not).
  const frames = useRef(new Map<string, Frame>());

  const update = useCallback((change: (next: Map<string, Detected>) => void) => {
    const next = new Map(entriesRef.current);
    change(next);
    entriesRef.current = next;
    setEntries(next);
  }, []);

  // The saved results, once the detection is on.
  useEffect(() => {
    if (!enabled || loaded) return;
    let current = true;
    void deps.store.all().then((all) => {
      if (!current) return;
      // Corrections made before the saved ones arrived stay.
      update((next) => {
        for (const [key, entry] of all) if (!next.has(key)) next.set(key, entry);
      });
      setLoaded(true);
    });
    return () => {
      current = false;
    };
  }, [enabled, loaded, deps.store, update]);

  // The pictures that have no result yet; again when the list of recordings changes.
  const signature = items
    ? `${items.length}:${items[0] ? recordingKey(items[0]) : ''}:${
        items.length > 0 ? recordingKey(items[items.length - 1] as Recording) : ''
      }`
    : '';
  useEffect(() => {
    if (!enabled || !loaded || !items || items.length === 0) return;
    let current = true;
    void (async () => {
      // Results of recordings that are gone do not pile up on the device (#459).
      const stale = staleKeys(entriesRef.current, items, now());
      if (stale.length > 0) {
        update((next) => {
          for (const key of stale) next.delete(key);
        });
        for (const key of stale) frames.current.delete(key);
        await deps.store.remove(stale);
        if (!current) return;
      }
      const byCamera = new Map<string, Recording[]>();
      for (const r of items) byCamera.set(r.camId, [...(byCamera.get(r.camId) ?? []), r]);
      const cameras = [...byCamera.values()];
      for (const [index, recordings] of cameras.entries()) {
        const { results } = await analyseCamera(recordings, {
          loadFrame: deps.loadFrame,
          known: entriesRef.current,
          now: now(),
          frames: frames.current,
          onProgress: (done, total) => {
            if (current) setProgress({ done, total, camera: index + 1, cameras: cameras.length });
          },
          isCancelled: () => !current,
        });
        if (!current) return;
        if (results.size === 0) continue;
        const merged: [string, Detected][] = [];
        update((next) => {
          for (const [key, entry] of results) {
            // A correction made meanwhile stays.
            const corrected = next.get(key)?.corrected;
            const value = corrected ? { ...entry, corrected } : entry;
            next.set(key, value);
            merged.push([key, value]);
          }
        });
        await Promise.all(merged.map(([key, value]) => deps.store.put(key, value)));
      }
      if (current) setProgress(undefined);
    })();
    return () => {
      current = false;
      setProgress(undefined);
    };
    // `items` is represented by its signature: the lists are rebuilt at every poll.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, loaded, signature, deps.loadFrame, deps.store]);

  const verdicts = useMemo(() => {
    const map = new Map<string, Verdict>();
    if (!enabled) return map;
    for (const [key, entry] of entries) map.set(key, verdictOf(entry, sensitivity));
    return map;
  }, [enabled, entries, sensitivity]);

  /** The recordings the user has given a verdict to. */
  const corrected = useMemo(() => {
    const keys = new Set<string>();
    for (const [key, entry] of entries) if (entry.corrected) keys.add(key);
    return keys;
  }, [entries]);

  const setEnabled = useCallback((on: boolean) => {
    write(ON_KEY, on ? 'on' : 'off');
    setEnabledState(on);
  }, []);

  const setSensitivity = useCallback((value: Sensitivity) => {
    write(SENSITIVITY_KEY, value);
    setSensitivityState(value);
  }, []);

  /** The user's word on a recording; `undefined` takes it back. */
  const correct = useCallback(
    (key: string, value: 'empty' | 'animal' | undefined) => {
      const before = entriesRef.current.get(key);
      const entry: Detected = {
        score: before?.score ?? 0,
        unsure: before?.unsure ?? true,
        ...(before?.reason && { reason: before.reason }),
        ...(before?.pool !== undefined && { pool: before.pool }),
        ...(before?.early && { early: true as const }),
        method: before?.method ?? METHOD,
        at: before?.at ?? now(),
        ...(value && { corrected: value }),
      };
      update((next) => next.set(key, entry));
      void deps.store.put(key, entry);
    },
    [deps.store, now, update],
  );

  return {
    enabled,
    setEnabled,
    sensitivity,
    setSensitivity,
    verdicts,
    corrected,
    progress,
    correct,
  };
}
