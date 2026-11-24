import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { analyseCamera, verdictOf, type Detected, type EmptyStore } from './emptyDetector';
import { METHOD, type Frame, type Sensitivity, type Verdict } from './emptiness';
import { recordingKey } from './grouping';
import type { Recording } from './types';

export interface EmptyDeps {
  loadFrame(url: string): Promise<Frame | undefined>;
  store: EmptyStore;
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
  const [progress, setProgress] = useState<{ done: number; total: number }>();
  const entriesRef = useRef(entries);
  useEffect(() => {
    entriesRef.current = entries;
  }, [entries]);

  // The saved results, once the detection is on.
  useEffect(() => {
    if (!enabled || loaded) return;
    let current = true;
    void deps.store.all().then((all) => {
      if (!current) return;
      setEntries(all);
      setLoaded(true);
    });
    return () => {
      current = false;
    };
  }, [enabled, loaded, deps.store]);

  // The pictures that have no result yet; again when the list of recordings changes.
  const signature = items ? `${items.length}:${items[0] ? recordingKey(items[0]) : ''}` : '';
  useEffect(() => {
    if (!enabled || !loaded || !items || items.length === 0) return;
    let current = true;
    void (async () => {
      const byCamera = new Map<string, Recording[]>();
      for (const r of items) byCamera.set(r.camId, [...(byCamera.get(r.camId) ?? []), r]);
      for (const recordings of byCamera.values()) {
        const { results } = await analyseCamera(recordings, {
          loadFrame: deps.loadFrame,
          known: entriesRef.current,
          now: now(),
          onProgress: (done, total) => {
            if (current) setProgress({ done, total });
          },
          isCancelled: () => !current,
        });
        if (!current) return;
        if (results.size > 0) {
          setEntries((before) => {
            const next = new Map(before);
            for (const [key, entry] of results) {
              // A correction made meanwhile stays.
              next.set(key, { ...entry, corrected: before.get(key)?.corrected });
            }
            return next;
          });
          for (const [key, entry] of results) {
            await deps.store.put(key, {
              ...entry,
              corrected: entriesRef.current.get(key)?.corrected,
            });
          }
        }
      }
      if (current) setProgress(undefined);
    })();
    return () => {
      current = false;
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
        method: before?.method ?? METHOD,
        at: before?.at ?? now(),
        corrected: value,
      };
      setEntries((all) => new Map(all).set(key, entry));
      void deps.store.put(key, entry);
    },
    [deps.store, now],
  );

  return { enabled, setEnabled, sensitivity, setSensitivity, verdicts, progress, correct };
}
