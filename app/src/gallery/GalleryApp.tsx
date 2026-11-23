import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import { ApiError } from '../api/http';
import type { CameraSettings } from '../api/types';
import { AdminView } from './AdminView';
import { showFaultBadge } from './badge';
import { FoundCameras } from './FoundCameras';
import { SettingsDialog } from './SettingsDialog';
import { hubStatusLine } from './hubStatus';
import {
  AlertIcon,
  CameraIcon,
  CheckIcon,
  ClockIcon,
  GearIcon,
  PencilIcon,
  PlayIcon,
  SaveIcon,
  SearchIcon,
  TrashIcon,
  UndoIcon,
} from './Icons';
import { Viewer } from './Viewer';
import * as realFolder from './folder';
import type { FolderState } from './folder';
import { batteryLevel } from '../format';
import {
  STATE_FILTERS,
  applyFilter,
  countByFilter,
  filterOf,
  groupByDay,
  newestFirst,
  recordingKey,
  type StateFilter,
} from './grouping';
import { badgeText, cameraClock, cameraHints, cameraTime, clockTime, folderName } from './text';
import {
  MAX_CAMERA_NAME,
  type FoundCamera,
  type GalleryApi,
  type GalleryCamera,
  type GalleryStatus,
  type Recording,
} from './types';

export interface FolderOps {
  folderState(): Promise<FolderState>;
  chooseFolder(): Promise<FolderState>;
  allowFolder(): Promise<FolderState>;
  saveToFolder(subFolder: string, fileName: string, data: Blob): Promise<void>;
  downloadFile(fileName: string, data: Blob): void;
}

interface Props {
  api: GalleryApi;
  folder?: FolderOps;
  now?: () => number;
  /** How often the page asks the backend for news, ms; 0 = never. */
  pollMs?: number;
}

const unixNow = () => Math.floor(Date.now() / 1000);
const SAVED_KEY = 'trailcamhub.gallery.saved';

// Which videos this laptop has saved already; storage may be blocked.
function loadSaved(): Set<number> {
  try {
    const raw = window.localStorage.getItem(SAVED_KEY);
    return new Set(raw ? (JSON.parse(raw) as number[]) : []);
  } catch {
    return new Set();
  }
}

function storeSaved(saved: Set<number>): void {
  try {
    window.localStorage.setItem(SAVED_KEY, JSON.stringify([...saved]));
  } catch {
    // Without storage the app saves again on the next visit; harmless.
  }
}

const errorText = (error: unknown) => (error instanceof Error ? error.message : String(error));

/** Recordings the user can choose for a joint action (#262). */
const selectable = (r: Recording) =>
  r.state === 'new' || r.state === 'failed' || r.state === 'videoReady';

/** Can still be asked for its video. */
const videoWanted = (r: Recording) =>
  r.fileType === 2 && (r.state === 'new' || r.state === 'failed');

/** Key of a found camera: a camera is known by its hub and BLE address. */
const foundKey = (f: FoundCamera) => `${f.hubId}/${f.bleAddress}`;

/**
 * Taken in, but the hub has not read it yet: it has no camId and no
 * recordings. The gallery shows it with the read cameras anyway, so it does
 * not vanish until the next scan.
 */
const notReachedYet = (f: FoundCamera) => f.state === 'approved' && !f.camId;

interface DeleteQuestion {
  question: string;
  items: Recording[];
}

/** "All cameras": the recordings of every shown camera, newest first (#431). */
const ALL = 'all';

const byCamera = (items: Recording[]): Map<string, Recording[]> => {
  const map = new Map<string, Recording[]>();
  for (const r of items) {
    const list = map.get(r.camId);
    if (list) list.push(r);
    else map.set(r.camId, [r]);
  }
  return map;
};

export function GalleryApp({ api, folder = realFolder, now = unixNow, pollMs = 15_000 }: Props) {
  const [status, setStatus] = useState<GalleryStatus>();
  const [camId, setCamId] = useState<string>();
  const [items, setItems] = useState<Recording[]>();
  const [loadError, setLoadError] = useState<string>();
  const [folderInfo, setFolderInfo] = useState<FolderState>({ kind: 'none' });
  const [saved, setSaved] = useState<Set<number>>(loadSaved);
  const [notice, setNotice] = useState<string>();
  const [confirm, setConfirm] = useState<DeleteQuestion>();
  // Chosen recordings by their key; ones that left the list are ignored.
  const [selected, setSelected] = useState<Set<string>>(() => new Set());
  // Selection mode: the checkboxes stay visible without hovering (#431).
  const [selecting, setSelecting] = useState(false);
  const [stateFilter, setStateFilter] = useState<StateFilter>('all');
  const [busy, setBusy] = useState(false);
  // The admin gets a second view with every hub (#264).
  const [isAdmin, setIsAdmin] = useState(false);
  const [adminView, setAdminView] = useState(false);
  const [faultCount, setFaultCount] = useState(0);
  // Asking before a camera is hidden (#273).
  const [hideAsk, setHideAsk] = useState<{ camId: string; name: string }>();
  // The tab "Kameras verwalten" instead of a camera's recordings.
  const [manage, setManage] = useState(false);
  // The camera whose name is being edited, in its tab or in the manage table.
  const [renaming, setRenaming] = useState<string>();
  // Cameras the hub heard (#298) and asking before one is no longer read.
  const [found, setFound] = useState<FoundCamera[]>([]);
  const [stopAsk, setStopAsk] = useState<FoundCamera>();
  // A camera taken in but not read yet, opened in its tab (foundKey).
  const [pendingKey, setPendingKey] = useState<string>();
  // A recording shown large, with its video if asked for (#306).
  const [viewing, setViewing] = useState<{ recording: Recording; video: boolean }>();
  // The camera whose settings dialog is open (#368).
  const [settingsFor, setSettingsFor] = useState<string>();
  const saving = useRef(false);
  // The chosen camera, readable in a reload that started before a switch: a
  // slow reload must not undo the user's choice (#431).
  const camIdRef = useRef<string | undefined>(undefined);
  const chooseCam = useCallback((id: string | undefined) => {
    camIdRef.current = id;
    setCamId(id);
  }, []);

  const reload = useCallback(async () => {
    try {
      // Both together, so a camera just read does not show twice: once read
      // and once still waiting. Older backends do not know found cameras:
      // then the list stays empty.
      const [s, f] = await Promise.all([
        api.status(),
        api.foundCameras().catch((): FoundCamera[] => []),
      ]);
      setStatus(s);
      setFound(f);
      const shown = s.cameras.filter((c) => !c.hidden);
      // "All cameras" is the default as soon as there is more than one.
      const current = camIdRef.current;
      let id: string | undefined;
      if (current === ALL && shown.length > 1) id = ALL;
      else if (current && current !== ALL && shown.some((c) => c.camId === current)) id = current;
      else id = shown.length > 1 ? ALL : shown[0]?.camId;
      chooseCam(id);
      const ids = id === ALL ? shown.map((c) => c.camId) : id ? [id] : [];
      const lists = await Promise.all(ids.map((c) => api.recordings(c)));
      // Switched meanwhile: the switch loads its own recordings.
      if (camIdRef.current !== id) return;
      setItems(newestFirst(lists.flat()));
      setLoadError(undefined);
    } catch (error) {
      setLoadError(error instanceof ApiError && error.status === 401 ? 'signin' : errorText(error));
    }
  }, [api, chooseCam]);

  useEffect(() => {
    void reload();
    if (pollMs <= 0) return;
    const timer = setInterval(() => void reload(), pollMs);
    return () => clearInterval(timer);
  }, [reload, pollMs]);

  useEffect(() => {
    // Older backends have no /v1/me: then there is no admin view.
    api.me().then(
      (me) => setIsAdmin(me.admin),
      () => setIsAdmin(false),
    );
  }, [api]);

  // The admin sees at a glance whether a hub needs him (#265).
  useEffect(() => {
    if (!isAdmin) return;
    api.adminHubs().then(
      (hubs) => setFaultCount(hubs.reduce((n, h) => n + h.faults.length, 0)),
      () => setFaultCount(0),
    );
  }, [api, isAdmin, adminView]);

  // …and on the app icon, if the browser can (#265).
  useEffect(() => {
    if (isAdmin) showFaultBadge(faultCount);
  }, [isAdmin, faultCount]);

  useEffect(() => {
    folder.folderState().then(setFolderInfo, () => setFolderInfo({ kind: 'unsupported' }));
  }, [folder]);

  const nameOf = (id: string) => status?.cameras.find((c) => c.camId === id)?.name ?? 'Kamera';
  const allCameras = camId === ALL;
  const shownCameras = status?.cameras.filter((c) => !c.hidden) ?? [];
  const currentCamera = allCameras ? undefined : status?.cameras.find((c) => c.camId === camId);
  const settingsCamera = status?.cameras.find((c) => c.camId === settingsFor);
  const notReached = found.filter(notReachedYet);
  // The open camera that waits for its first read; without a read camera
  // the first waiting one is open.
  const openPending = manage
    ? undefined
    : (notReached.find((f) => foundKey(f) === pendingKey) ??
      (shownCameras.length === 0 ? notReached[0] : undefined));
  // A read camera's recordings are shown.
  const showCamera = !manage && !openPending;

  // Once the hub has read the open waiting camera, its tab turns into a
  // normal one: open that, with its recordings.
  useEffect(() => {
    if (pendingKey === undefined) return;
    const entry = found.find((f) => foundKey(f) === pendingKey);
    if (entry && notReachedYet(entry)) return;
    setPendingKey(undefined);
    if (entry?.camId && status?.cameras.some((c) => c.camId === entry.camId && !c.hidden)) {
      chooseCam(entry.camId);
      setItems(undefined);
      void reload();
    }
  }, [found, pendingKey, status, chooseCam, reload]);

  const decide = async (camera: FoundCamera, state: 'approved' | 'excluded', name?: string) => {
    setBusy(true);
    try {
      await api.decideCamera(camera.hubId, camera.bleAddress, state, name);
      const shown = name ?? camera.name;
      setNotice(
        state === 'approved'
          ? `Die Kamera „${shown}“ wird ab dem nächsten Kontakt mit dem Hub gelesen.${
              camera.camId ? '' : ' Bis dahin steht sie ohne Aufnahmen bei Ihren Kameras.'
            }`
          : `Die Kamera „${shown}“ wird nicht mehr gelesen. Sie steht unten unter „Nicht abgefragte Kameras“.`,
      );
      await reload();
    } catch (error) {
      setNotice(`Das hat nicht geklappt: ${errorText(error)}`);
    } finally {
      setBusy(false);
    }
  };

  const requestScan = async () => {
    setBusy(true);
    try {
      const { requested, nextContact } = await api.requestScan();
      setNotice(
        requested > 0
          ? `Der Hub sucht beim nächsten Kontakt${
              nextContact ? ` (gegen ${clockTime(nextContact, now())})` : ''
            } nach Kameras. Neue Kameras erscheinen danach hier unten.`
          : 'Der Hub sucht bei jedem geplanten Lesen ohnehin nach Kameras. Neue erscheinen danach hier unten.',
      );
    } catch (error) {
      setNotice(`Das hat nicht geklappt: ${errorText(error)}`);
    } finally {
      setBusy(false);
    }
  };

  const setHidden = async (id: string, name: string, hidden: boolean) => {
    setBusy(true);
    try {
      await api.setCameraHidden(id, hidden);
      setNotice(
        hidden
          ? `Die Kamera „${name}“ ist ausgeblendet. Ihre Aufnahmen bleiben gespeichert.`
          : `Die Kamera „${name}“ wird wieder angezeigt.`,
      );
      await reload();
      // After the reload, which keeps the camera shown before; changing the
      // camera loads its recordings.
      if (!hidden) {
        chooseCam(id);
        setItems(undefined);
        await reload();
      }
    } catch (error) {
      setNotice(`Das hat nicht geklappt: ${errorText(error)}`);
    } finally {
      setBusy(false);
    }
  };

  const markSaved = useCallback((ids: number[]) => {
    setSaved((current) => {
      const next = new Set(current);
      ids.forEach((id) => next.add(id));
      storeSaved(next);
      return next;
    });
  }, []);

  // Saves every finished video that is not on this laptop yet.
  useEffect(() => {
    if (!items || folderInfo.kind !== 'ready' || saving.current) return;
    const pending = items.filter((r) => r.state === 'videoReady' && !saved.has(r.mediaId));
    if (pending.length === 0) return;
    saving.current = true;
    const nameFor = (id: string) => status?.cameras.find((c) => c.camId === id)?.name ?? 'Kamera';
    void (async () => {
      const done: number[] = [];
      try {
        for (const r of pending) {
          const data = await api.download(r);
          await folder.saveToFolder(folderName(nameFor(r.camId)), r.fileName, data);
          done.push(r.mediaId);
        }
      } catch (error) {
        setNotice(`Speichern hat nicht geklappt: ${errorText(error)}`);
        folder.folderState().then(setFolderInfo, () => undefined);
      } finally {
        if (done.length > 0) {
          markSaved(done);
          setNotice(
            done.length === 1
              ? `1 Video gespeichert im Ordner „${folderInfo.name}“.`
              : `${done.length} Videos gespeichert im Ordner „${folderInfo.name}“.`,
          );
        }
        saving.current = false;
      }
    })();
  }, [items, folderInfo, saved, api, folder, status, markSaved]);

  const act = async (run: () => Promise<void>, success?: string) => {
    setBusy(true);
    try {
      await run();
      if (success) setNotice(success);
      await reload();
    } catch (error) {
      setNotice(`Das hat nicht geklappt: ${errorText(error)}`);
    } finally {
      setBusy(false);
    }
  };

  const saveOne = async (r: Recording) => {
    try {
      const data = await api.download(r);
      if (folderInfo.kind === 'ready') {
        await folder.saveToFolder(folderName(nameOf(r.camId)), r.fileName, data);
        setNotice(`Video gespeichert im Ordner „${folderInfo.name}“.`);
      } else {
        folder.downloadFile(r.fileName, data);
        setNotice('Video wird heruntergeladen.');
      }
      markSaved([r.mediaId]);
    } catch (error) {
      setNotice(`Speichern hat nicht geklappt: ${errorText(error)}`);
    }
  };

  // Returns whether the name was stored, so the form can close.
  const rename = async (run: () => Promise<string>): Promise<boolean> => {
    setBusy(true);
    try {
      const stored = await run();
      setNotice(
        `Die Kamera heißt jetzt „${stored}“. Neue Videos landen im Unterordner „${folderName(stored)}“.`,
      );
      setRenaming(undefined);
      await reload();
      return true;
    } catch (error) {
      setNotice(`Umbenennen hat nicht geklappt: ${errorText(error)}`);
      return false;
    } finally {
      setBusy(false);
    }
  };

  const renameCamera = (id: string, name: string) => rename(() => api.renameCamera(id, name));

  // Returns whether the change was stored, so the dialog can close (#368).
  const saveSettings = async (id: string, changed: CameraSettings): Promise<boolean> => {
    setBusy(true);
    try {
      await api.changeSettings(id, changed);
      setNotice('Die Einstellungen gehen beim nächsten Kontakt mit dem Hub an die Kamera.');
      await reload();
      return true;
    } catch (error) {
      setNotice(`Speichern hat nicht geklappt: ${errorText(error)}`);
      return false;
    } finally {
      setBusy(false);
    }
  };

  const withdrawSettings = async (id: string) => {
    setBusy(true);
    try {
      const withdrawn = await api.withdrawSettings(id);
      setNotice(
        withdrawn
          ? 'Die Änderung ist zurückgenommen.'
          : 'Der Hub hat die Änderung schon erhalten, Zurücknehmen geht nicht mehr.',
      );
      await reload();
    } catch (error) {
      setNotice(`Das hat nicht geklappt: ${errorText(error)}`);
    } finally {
      setBusy(false);
    }
  };
  // A camera not read yet keeps the name and takes it at its first read.
  const renamePending = (camera: FoundCamera, name: string) =>
    rename(async () => {
      await api.decideCamera(camera.hubId, camera.bleAddress, 'approved', name);
      return name;
    });

  const pickFolder = async (allow: boolean) => {
    try {
      setFolderInfo(await (allow ? folder.allowFolder() : folder.chooseFolder()));
    } catch {
      // The user closed the dialog; nothing to report.
    }
  };

  const toggle = (key: string) =>
    setSelected((current) => {
      const next = new Set(current);
      if (!next.delete(key)) next.add(key);
      return next;
    });

  const clearSelection = () => {
    setSelected(new Set());
    setSelecting(false);
  };

  // Several cameras at once: one request per camera.
  const requestVideosFor = async (list: Recording[]) => {
    for (const [id, rs] of byCamera(list)) {
      await api.requestVideos(
        id,
        rs.map((r) => r.mediaId),
      );
    }
  };
  const requestDeleteFor = async (list: Recording[]) => {
    for (const [id, rs] of byCamera(list)) {
      await api.requestDelete(
        id,
        rs.flatMap((r) => r.mediaIds),
      );
    }
  };

  const t = now();
  const counts = countByFilter(items ?? []);
  const visible = applyFilter(items ?? [], stateFilter);
  const days = groupByDay(visible, t);
  const chosen = items?.filter((r) => selected.has(recordingKey(r)) && selectable(r)) ?? [];
  const chosenVideos = chosen.filter(videoWanted);

  const newFound = found.filter((f) => f.state === 'new').length;
  const switchCamera = (id: string) => {
    setManage(false);
    setRenaming(undefined);
    setPendingKey(undefined);
    if (id !== camIdRef.current) {
      chooseCam(id);
      setItems(undefined);
      clearSelection();
      setStateFilter('all');
      void reload();
    }
  };
  const openWaiting = (camera: FoundCamera) => {
    setManage(false);
    setRenaming(undefined);
    setPendingKey(foundKey(camera));
    setSelected(new Set());
  };

  return (
    <div className="gallery">
      <header className="g-header">
        <div className="g-brand">
          <span className="g-logo" aria-hidden="true">
            <CameraIcon />
          </span>
          <h1>Wildkamera</h1>
        </div>
        {status && <HubChip status={status} now={t} />}
        {isAdmin && (
          <button
            type="button"
            className="g-btn g-btn--quiet g-header__switch"
            aria-pressed={adminView}
            onClick={() => setAdminView((on) => !on)}
          >
            {adminView
              ? 'Zur Galerie'
              : faultCount > 0
                ? `Verwaltung (${faultCount})`
                : 'Verwaltung'}
          </button>
        )}
      </header>

      {adminView ? (
        <AdminView api={api} now={now} />
      ) : (
        <main className="g-main">
          {api.isDemo && (
            <p className="g-banner" role="note">
              Vorführung: Beispielbilder, kein echter Hub. Der Hub „meldet sich“ hier alle 20
              Sekunden.
            </p>
          )}

          <FolderBar
            info={folderInfo}
            onChoose={() => void pickFolder(false)}
            onAllow={() => void pickFolder(true)}
          />

          {notice && (
            <p className="g-notice" role="status">
              <span>{notice}</span>
              <button type="button" className="g-link" onClick={() => setNotice(undefined)}>
                Ausblenden
              </button>
            </p>
          )}

          {loadError && (
            <div className="g-error" role="alert">
              <p>
                {loadError === 'signin'
                  ? 'Die Anmeldung gilt nicht mehr. Bitte den neuen Einrichtungslink öffnen.'
                  : 'Die Bilder konnten nicht geladen werden. Ist der Laptop mit dem Internet verbunden?'}
              </p>
              <button type="button" className="g-btn" onClick={() => void reload()}>
                Nochmal versuchen
              </button>
            </div>
          )}

          {status && (
            <div className="g-filters">
              <div className="g-chips" role="tablist" aria-label="Kameras">
                {shownCameras.length > 1 && (
                  <button
                    type="button"
                    role="tab"
                    aria-selected={showCamera && allCameras}
                    className="g-chip"
                    onClick={() => switchCamera(ALL)}
                  >
                    Alle Kameras
                  </button>
                )}
                {shownCameras.map((c) => {
                  const active = showCamera && c.camId === camId;
                  if (active && renaming === c.camId) {
                    return (
                      <RenameForm
                        key={c.camId}
                        name={c.name}
                        disabled={busy}
                        onRename={(name) => renameCamera(c.camId, name)}
                        onCancel={() => setRenaming(undefined)}
                      />
                    );
                  }
                  const read = c.batPercent !== undefined && (c.lastRead ?? 0) > 0;
                  const low = read && batteryLevel(c.batPercent ?? 100) !== 'ok';
                  return (
                    <span key={c.camId} className="g-chip-wrap">
                      <button
                        type="button"
                        role="tab"
                        aria-selected={active}
                        aria-label={c.name}
                        className="g-chip"
                        onClick={() => switchCamera(c.camId)}
                      >
                        {c.name}
                        {read && (
                          <span
                            className={low ? 'g-chip__bat g-chip__bat--low' : 'g-chip__bat'}
                            title={low ? 'Akku schwach' : 'Akku'}
                          >
                            {c.batPercent} %{low && ' ⚠'}
                          </span>
                        )}
                      </button>
                      {active && (
                        <RenameButton
                          label="Umbenennen"
                          disabled={busy}
                          onClick={() => setRenaming(c.camId)}
                        />
                      )}
                    </span>
                  );
                })}
                {notReached.map((f) => {
                  const key = foundKey(f);
                  const active = openPending === f;
                  if (active && renaming === key) {
                    return (
                      <RenameForm
                        key={key}
                        name={f.name}
                        disabled={busy}
                        onRename={(name) => renamePending(f, name)}
                        onCancel={() => setRenaming(undefined)}
                      />
                    );
                  }
                  return (
                    <span key={key} className="g-chip-wrap">
                      <button
                        type="button"
                        role="tab"
                        aria-selected={active}
                        className="g-chip g-chip--waiting"
                        onClick={() => openWaiting(f)}
                      >
                        {f.name} <span className="g-chip__note">noch nicht erreicht</span>
                      </button>
                      {active && (
                        <RenameButton
                          label="Umbenennen"
                          disabled={busy}
                          onClick={() => setRenaming(key)}
                        />
                      )}
                    </span>
                  );
                })}
                <button
                  type="button"
                  role="tab"
                  aria-selected={manage}
                  className="g-chip g-chip--manage"
                  onClick={() => {
                    setManage(true);
                    setRenaming(undefined);
                    setPendingKey(undefined);
                  }}
                >
                  <GearIcon /> Kameras verwalten
                  {newFound > 0 && (
                    <span className="g-count" aria-label={`${newFound} neu`}>
                      {newFound}
                    </span>
                  )}
                </button>
              </div>
            </div>
          )}

          {showCamera && items && items.length > 0 && (
            <div className="g-subfilters">
              <div className="g-statefilter" role="group" aria-label="Zustand">
                {STATE_FILTERS.filter(
                  (f) => f.id === 'all' || counts[f.id] > 0 || stateFilter === f.id,
                ).map((f) => (
                  <button
                    key={f.id}
                    type="button"
                    aria-pressed={stateFilter === f.id}
                    className={`g-sfilter g-sfilter--${f.id}`}
                    onClick={() => setStateFilter(f.id)}
                  >
                    {f.id !== 'all' && <i className="g-sfilter__dot" aria-hidden="true" />}
                    {f.label}
                    {f.id !== 'all' && <span className="g-sfilter__n"> {counts[f.id]}</span>}
                  </button>
                ))}
              </div>
              <button
                type="button"
                className="g-btn g-btn--quiet g-btn--small"
                aria-pressed={selecting}
                onClick={() => (selecting ? clearSelection() : setSelecting(true))}
              >
                {selecting ? 'Fertig' : 'Auswählen'}
              </button>
            </div>
          )}

          {status && manage && (
            <section className="g-panel g-manage" aria-label="Kameras verwalten">
              <div className="g-section-head">
                <h2>Ihre Kameras</h2>
                <button
                  type="button"
                  className="g-btn g-btn--quiet"
                  disabled={busy}
                  onClick={() => void requestScan()}
                >
                  <SearchIcon /> Nach Kameras suchen
                </button>
              </div>
              {status.cameras.length === 0 && notReached.length === 0 ? (
                <p className="g-hint">
                  {isAdmin && !api.elevated()
                    ? 'Dieses Konto hat keinen verknüpften Hub. Unter „Verwaltung“ den Code eingeben, dann sind alle Hubs sichtbar.'
                    : 'Der Hub hat noch keine Kamera gelesen.'}
                </p>
              ) : (
                <table className="g-table g-cams">
                  <thead>
                    <tr>
                      <th scope="col">Kamera</th>
                      <th scope="col">Zustand</th>
                      <th scope="col">
                        <span className="g-visually-hidden">Aktionen</span>
                      </th>
                    </tr>
                  </thead>
                  <tbody>
                    {status.cameras.map((c) => {
                      const entry = found.find((f) => f.camId === c.camId);
                      const state = [
                        c.hidden ? 'ausgeblendet' : 'in der Galerie',
                        entry?.state === 'excluded'
                          ? 'wird nicht abgefragt'
                          : entry
                            ? 'wird abgefragt'
                            : '',
                        c.settingsState ? 'Einstellungen werden geändert' : '',
                      ].filter(Boolean);
                      return (
                        <tr key={c.camId}>
                          <th scope="row">
                            {renaming === c.camId ? (
                              <RenameForm
                                name={c.name}
                                disabled={busy}
                                onRename={(name) => renameCamera(c.camId, name)}
                                onCancel={() => setRenaming(undefined)}
                              />
                            ) : (
                              <span className="g-cams__name">
                                {c.name}
                                <RenameButton
                                  label={`${c.name} umbenennen`}
                                  disabled={busy}
                                  onClick={() => setRenaming(c.camId)}
                                />
                              </span>
                            )}
                          </th>
                          <td>{state.join(' · ')}</td>
                          <td>
                            <div className="g-cams__actions">
                              {c.settings && (
                                <button
                                  type="button"
                                  className="g-btn g-btn--quiet"
                                  disabled={busy}
                                  onClick={() => setSettingsFor(c.camId)}
                                >
                                  Einstellungen
                                </button>
                              )}
                              {c.hidden ? (
                                <button
                                  type="button"
                                  className="g-btn g-btn--quiet"
                                  disabled={busy}
                                  onClick={() => void setHidden(c.camId, c.name, false)}
                                >
                                  Wieder anzeigen
                                </button>
                              ) : (
                                <button
                                  type="button"
                                  className="g-btn g-btn--quiet"
                                  disabled={busy}
                                  onClick={() => setHideAsk({ camId: c.camId, name: c.name })}
                                >
                                  Ausblenden
                                </button>
                              )}
                              {entry?.state === 'approved' && (
                                <button
                                  type="button"
                                  className="g-btn g-btn--quiet"
                                  disabled={busy}
                                  onClick={() => setStopAsk({ ...entry, name: c.name })}
                                >
                                  Nicht mehr abfragen
                                </button>
                              )}
                              {entry?.state === 'excluded' && (
                                <button
                                  type="button"
                                  className="g-btn g-btn--quiet"
                                  disabled={busy}
                                  onClick={() =>
                                    void decide({ ...entry, name: c.name }, 'approved')
                                  }
                                >
                                  Doch aufnehmen
                                </button>
                              )}
                            </div>
                          </td>
                        </tr>
                      );
                    })}
                    {notReached.map((f) => {
                      const key = foundKey(f);
                      return (
                        <tr key={key}>
                          <th scope="row">
                            {renaming === key ? (
                              <RenameForm
                                name={f.name}
                                disabled={busy}
                                onRename={(name) => renamePending(f, name)}
                                onCancel={() => setRenaming(undefined)}
                              />
                            ) : (
                              <span className="g-cams__name">
                                {f.name}
                                <RenameButton
                                  label={`${f.name} umbenennen`}
                                  disabled={busy}
                                  onClick={() => setRenaming(key)}
                                />
                              </span>
                            )}
                          </th>
                          <td>in der Galerie · noch nicht erreicht</td>
                          <td>
                            <div className="g-cams__actions">
                              <button
                                type="button"
                                className="g-btn g-btn--quiet"
                                disabled={busy}
                                onClick={() => setStopAsk(f)}
                              >
                                Nicht mehr abfragen
                              </button>
                            </div>
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              )}
              <FoundCameras
                found={found.filter((f) => !f.camId)}
                busy={busy}
                now={t}
                onDecide={(c, st, n) => void decide(c, st, n)}
              />
            </section>
          )}

          {status && openPending && (
            <WaitingCamera camera={openPending} nextContact={status.nextContact} now={t} />
          )}

          {showCamera && status && camId && !allCameras && (
            <div className="g-camera-bar">
              {/* The chip shows the name; the heading is for screen readers. */}
              <h2 className="g-visually-hidden">Kamera „{nameOf(camId)}“</h2>
              {currentCamera && <CameraFacts camera={currentCamera} now={t} />}
            </div>
          )}
          {showCamera && status && allCameras && (
            <h2 className="g-visually-hidden">Alle Kameras</h2>
          )}
          {showCamera && status && currentCamera && (
            <CameraHints camera={currentCamera} hubLastContact={status.lastContact} />
          )}
          {showCamera && status && allCameras && (
            <AllCameraHints cameras={shownCameras} hubLastContact={status.lastContact} />
          )}

          {showCamera && chosen.length > 0 && (
            <div className="g-selbar" role="region" aria-label="Auswahl">
              <p>
                {chosen.length === 1
                  ? '1 Aufnahme ausgewählt'
                  : `${chosen.length} Aufnahmen ausgewählt`}
              </p>
              <div className="g-selbar__actions">
                {chosenVideos.length > 0 && (
                  <button
                    type="button"
                    className="g-btn"
                    disabled={busy}
                    onClick={() =>
                      void act(async () => {
                        await requestVideosFor(chosenVideos);
                        clearSelection();
                      })
                    }
                  >
                    Videos holen ({chosenVideos.length})
                  </button>
                )}
                <button
                  type="button"
                  className="g-btn g-btn--danger"
                  disabled={busy}
                  onClick={() =>
                    setConfirm({
                      question:
                        chosen.length === 1
                          ? '1 ausgewählte Aufnahme löschen?'
                          : `${chosen.length} ausgewählte Aufnahmen löschen?`,
                      items: chosen,
                    })
                  }
                >
                  Löschen ({chosen.length})
                </button>
                <button type="button" className="g-btn g-btn--quiet" onClick={clearSelection}>
                  Auswahl aufheben
                </button>
              </div>
            </div>
          )}

          {showCamera && items === undefined && !loadError && (
            <p className="g-hint">Bilder werden geladen …</p>
          )}
          {showCamera && items && items.length === 0 && (
            <p className="g-empty">
              Keine Aufnahmen. Neue Bilder erscheinen nach dem nächsten Kontakt.
            </p>
          )}

          {showCamera && items && items.length > 0 && visible.length === 0 && (
            <p className="g-empty">
              Keine Aufnahmen mit diesem Filter.{' '}
              <button type="button" className="g-link" onClick={() => setStateFilter('all')}>
                Filter zurücksetzen
              </button>
            </p>
          )}

          {showCamera &&
            items &&
            days.map((group) => (
              <section key={group.day} className="g-day" aria-label={group.title}>
                <div className="g-day__head">
                  <h2>{group.title}</h2>
                  <span>
                    {group.subtitle && `${group.subtitle} · `}
                    {group.items.length === 1 ? '1 Aufnahme' : `${group.items.length} Aufnahmen`}
                  </span>
                  {group.items.some(selectable) && (
                    <button
                      type="button"
                      className="g-btn g-btn--quiet g-btn--small g-day__select"
                      aria-label={`Alle auswählen: ${group.title}`}
                      onClick={() => {
                        setSelecting(true);
                        setSelected((current) => {
                          const next = new Set(current);
                          group.items.filter(selectable).forEach((r) => next.add(recordingKey(r)));
                          return next;
                        });
                      }}
                    >
                      Alle auswählen
                    </button>
                  )}
                </div>
                <ul className="g-grid" aria-label={`Aufnahmen ${group.title}`}>
                  {group.items.map((r) => {
                    const key = recordingKey(r);
                    const picked = selected.has(key) && selectable(r);
                    return (
                      <li
                        key={key}
                        className={`g-tile g-tile--${r.state}${picked ? ' g-tile--chosen' : ''}${
                          selecting || selected.size > 0 ? ' g-tile--selecting' : ''
                        }`}
                      >
                        <button
                          type="button"
                          className="g-tile__zoom"
                          aria-label={`Groß anzeigen: ${cameraTime(r.mediaTime)}`}
                          onClick={() => setViewing({ recording: r, video: false })}
                        >
                          <img src={r.thumbUrl} alt={`Aufnahme vom ${cameraTime(r.mediaTime)}`} />
                        </button>
                        {selectable(r) && (
                          <label className="g-tile__select" title="Auswählen">
                            <input
                              type="checkbox"
                              checked={selected.has(key)}
                              onChange={() => toggle(key)}
                            />
                            <span className="g-visually-hidden">Auswählen</span>
                          </label>
                        )}
                        <span
                          className={`g-tile__state g-tile__state--${filterOf(r.state) ?? 'new'}`}
                          title={r.failReason}
                        >
                          <StateIcon state={r.state} />
                          {badgeText(r.state, saved.has(r.mediaId), status?.nextContact, t)}
                          {r.state === 'failed' && r.failReason && (
                            <span className="g-visually-hidden"> – {r.failReason}</span>
                          )}
                        </span>
                        {r.state === 'videoReady' && (
                          <button
                            type="button"
                            className="g-tile__play"
                            aria-label="Video ansehen"
                            title="Video ansehen"
                            onClick={() => setViewing({ recording: r, video: true })}
                          >
                            <PlayIcon />
                          </button>
                        )}
                        {r.state === 'videoRequested' && (
                          <span className="g-tile__progress" aria-hidden="true" />
                        )}
                        <div className="g-tile__meta">
                          <span className="g-tile__time">{cameraClock(r.mediaTime)}</span>
                          {allCameras && <span className="g-tile__cam">{nameOf(r.camId)}</span>}
                          {r.fileType === 0 && <span className="g-tile__cam">Foto</span>}
                        </div>
                        <div className="g-tile__actions">
                          {videoWanted(r) && (
                            <button
                              type="button"
                              className="g-btn g-btn--small g-tile__main"
                              disabled={busy}
                              onClick={() =>
                                void act(() => api.requestVideos(r.camId, [r.mediaId]))
                              }
                            >
                              Video holen
                            </button>
                          )}
                          <span className="g-tile__spacer" />
                          {r.state === 'videoReady' && (
                            <IconButton
                              label={saved.has(r.mediaId) ? 'Nochmal speichern' : 'Video speichern'}
                              onClick={() => void saveOne(r)}
                            >
                              <SaveIcon />
                            </IconButton>
                          )}
                          {(r.state === 'videoRequested' || r.state === 'deleteRequested') && (
                            <IconButton
                              label="Zurücknehmen"
                              disabled={busy}
                              onClick={() => void act(() => api.withdraw(r.camId, r))}
                            >
                              <UndoIcon />
                            </IconButton>
                          )}
                          {selectable(r) && (
                            <IconButton
                              label="Löschen"
                              danger
                              disabled={busy}
                              onClick={() => void act(() => api.requestDelete(r.camId, r.mediaIds))}
                            >
                              <TrashIcon />
                            </IconButton>
                          )}
                        </div>
                      </li>
                    );
                  })}
                </ul>
              </section>
            ))}

          {viewing && (
            <Viewer
              recording={viewing.recording}
              video={viewing.video}
              onClose={() => setViewing(undefined)}
            />
          )}

          {settingsCamera && (
            <SettingsDialog
              key={settingsCamera.camId}
              camera={settingsCamera}
              busy={busy}
              onSave={(changed) => saveSettings(settingsCamera.camId, changed)}
              onWithdraw={() => void withdrawSettings(settingsCamera.camId)}
              onClose={() => setSettingsFor(undefined)}
            />
          )}

          {hideAsk && (
            <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-hide">
              <div className="g-dialog__box">
                <p id="g-hide">Kamera „{hideAsk.name}“ ausblenden?</p>
                <p className="g-hint">
                  Ihre Aufnahmen bleiben gespeichert, und der Hub liest sie weiter. Unter „Kameras
                  verwalten“ können Sie die Kamera wieder anzeigen.
                </p>
                <div className="g-dialog__actions">
                  <button
                    type="button"
                    className="g-btn g-btn--quiet"
                    onClick={() => setHideAsk(undefined)}
                  >
                    Abbrechen
                  </button>
                  <button
                    type="button"
                    className="g-btn g-btn--danger"
                    onClick={() => {
                      const { camId: id, name } = hideAsk;
                      setHideAsk(undefined);
                      if (id === camId) {
                        setItems(undefined);
                        setSelected(new Set());
                      }
                      void setHidden(id, name, true);
                    }}
                  >
                    Ja, ausblenden
                  </button>
                </div>
              </div>
            </div>
          )}

          {stopAsk && (
            <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-stop">
              <div className="g-dialog__box">
                <p id="g-stop">Kamera „{stopAsk.name}“ nicht mehr abfragen?</p>
                <p className="g-hint">
                  {stopAsk.camId
                    ? 'Der Hub liest sie ab dem nächsten Kontakt nicht mehr. Die bisherigen Aufnahmen bleiben hier. Unter „Kameras verwalten“ können Sie sie wieder aufnehmen.'
                    : 'Der Hub liest sie dann nicht. Unter „Kameras verwalten“ können Sie sie wieder aufnehmen.'}
                </p>
                <div className="g-dialog__actions">
                  <button
                    type="button"
                    className="g-btn g-btn--quiet"
                    onClick={() => setStopAsk(undefined)}
                  >
                    Abbrechen
                  </button>
                  <button
                    type="button"
                    className="g-btn g-btn--danger"
                    onClick={() => {
                      const camera = stopAsk;
                      setStopAsk(undefined);
                      void decide(camera, 'excluded');
                    }}
                  >
                    Ja, nicht mehr abfragen
                  </button>
                </div>
              </div>
            </div>
          )}

          {confirm && (
            <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-confirm">
              <div className="g-dialog__box">
                <p id="g-confirm">{confirm.question}</p>
                <p className="g-hint">
                  Bis zum nächsten Kontakt mit dem Hub lässt sich das noch zurücknehmen.
                </p>
                <div className="g-dialog__actions">
                  <button
                    type="button"
                    className="g-btn g-btn--quiet"
                    onClick={() => setConfirm(undefined)}
                  >
                    Abbrechen
                  </button>
                  <button
                    type="button"
                    className="g-btn g-btn--danger"
                    onClick={() => {
                      setConfirm(undefined);
                      clearSelection();
                      void act(
                        () => requestDeleteFor(confirm.items),
                        'Die Aufnahmen werden beim nächsten Kontakt gelöscht.',
                      );
                    }}
                  >
                    Ja, löschen
                  </button>
                </div>
              </div>
            </div>
          )}
        </main>
      )}
    </div>
  );
}

/**
 * The hub's state as one chip (#431): dot, word and next contact; a tap opens
 * the facts the old status line showed.
 */
function HubChip({ status, now }: { status: GalleryStatus; now: number }) {
  const line = hubStatusLine(status.lastContact, status.nextContact, now);
  return (
    <details className="g-hubchip">
      <summary>
        <span className={`g-dot g-dot--${line.light}`} aria-hidden="true" />
        <span className="g-hubchip__title">{line.title}</span>
        {line.next && <span className="g-hubchip__next">· nächster Kontakt {line.next}</span>}
      </summary>
      <dl className="g-hubchip__facts">
        <dt>Letzter Kontakt</dt>
        <dd>{status.lastContact > 0 ? clockTime(status.lastContact, now) : 'noch keiner'}</dd>
        {status.nextContact !== undefined && (
          <>
            <dt>Nächster Kontakt</dt>
            <dd>gegen {clockTime(status.nextContact, now)}</dd>
          </>
        )}
        {status.hubBatPercent !== undefined && (
          <>
            <dt>Akku des Hubs</dt>
            <dd>{status.hubBatPercent} %</dd>
          </>
        )}
      </dl>
    </details>
  );
}

/** A small button with an icon only; the label is its name and tooltip. */
function IconButton({
  label,
  danger,
  disabled,
  onClick,
  children,
}: {
  label: string;
  danger?: boolean;
  disabled?: boolean;
  onClick(): void;
  children: ReactNode;
}) {
  return (
    <button
      type="button"
      className={danger ? 'g-icon-btn g-icon-btn--danger' : 'g-icon-btn'}
      aria-label={label}
      title={label}
      disabled={disabled}
      onClick={onClick}
    >
      {children}
    </button>
  );
}

/** Battery and last read of the open camera (#304). */
function CameraFacts({ camera, now }: { camera: GalleryCamera; now: number }) {
  const facts = [
    camera.batPercent !== undefined && (camera.lastRead ?? 0) > 0
      ? `Akku ${camera.batPercent} %`
      : '',
    (camera.lastRead ?? 0) > 0 ? `zuletzt gelesen ${clockTime(camera.lastRead ?? 0, now)}` : '',
  ].filter(Boolean);
  if (facts.length === 0) return null;
  return <p className="g-camera-state">{facts.join(' · ')}</p>;
}

/**
 * A camera taken in that the hub has not read yet: shown like a read one,
 * without recordings, and with a word on when they come.
 */
function WaitingCamera({
  camera,
  nextContact,
  now,
}: {
  camera: FoundCamera;
  nextContact: number | undefined;
  now: number;
}) {
  return (
    <>
      <div className="g-camera-bar">
        <h2 className="g-visually-hidden">Kamera „{camera.name}“</h2>
        <p className="g-camera-state">
          {camera.lastSeen > 0
            ? `noch nicht gelesen · zuletzt gehört ${clockTime(camera.lastSeen, now)}`
            : 'noch nicht gelesen'}
        </p>
      </div>
      <div className="g-banner g-camera-hints" role="status" aria-label="Hinweise zur Kamera">
        <p>
          Der Hub hat diese Kamera noch nicht erreicht. Er liest sie beim nächsten Kontakt
          {nextContact !== undefined && ` (gegen ${clockTime(nextContact, now)})`}, danach
          erscheinen hier ihre Aufnahmen.
        </p>
      </div>
      <p className="g-empty">Noch keine Aufnahmen.</p>
    </>
  );
}

/** The icon in a tile's state badge (#431). */
function StateIcon({ state }: { state: Recording['state'] }) {
  switch (state) {
    case 'new':
      return <i className="g-tile__dot" aria-hidden="true" />;
    case 'videoRequested':
      return <ClockIcon />;
    case 'videoReady':
      return <CheckIcon />;
    case 'deleteRequested':
    case 'deleted':
      return <TrashIcon />;
    case 'failed':
      return <AlertIcon />;
  }
}

/** The hints of every shown camera, for the view of all cameras (#431). */
function AllCameraHints({
  cameras,
  hubLastContact,
}: {
  cameras: GalleryCamera[];
  hubLastContact: number;
}) {
  const hints = cameras.flatMap((c) =>
    cameraHints(c, hubLastContact).map((h) => `Kamera „${c.name}“: ${h}`),
  );
  if (hints.length === 0) return null;
  return (
    <div className="g-banner g-camera-hints" role="status" aria-label="Hinweise zu den Kameras">
      {hints.map((h) => (
        <p key={h}>{h}</p>
      ))}
    </div>
  );
}

/** What needs a look at the open camera (#304). */
function CameraHints({
  camera,
  hubLastContact,
}: {
  camera: GalleryCamera;
  hubLastContact: number;
}) {
  const hints = cameraHints(camera, hubLastContact);
  if (hints.length === 0) return null;
  return (
    <div className="g-banner g-camera-hints" role="status" aria-label="Hinweise zur Kamera">
      {hints.map((h) => (
        <p key={h}>{h}</p>
      ))}
    </div>
  );
}

/** The pencil next to a camera's name (#273). */
function RenameButton({
  label,
  disabled,
  onClick,
}: {
  label: string;
  disabled: boolean;
  onClick(): void;
}) {
  return (
    <button
      type="button"
      className="g-icon-btn g-icon-btn--small"
      aria-label={label}
      title="Umbenennen"
      disabled={disabled}
      onClick={onClick}
    >
      <PencilIcon />
    </button>
  );
}

/** A camera's name in place, to change it (#273). */
function RenameForm({
  name,
  disabled,
  onRename,
  onCancel,
}: {
  name: string;
  disabled: boolean;
  onRename(name: string): Promise<boolean>;
  onCancel(): void;
}) {
  const [value, setValue] = useState(name);
  const trimmed = value.trim();
  return (
    <form
      className="g-rename g-rename--inline"
      onSubmit={(event) => {
        event.preventDefault();
        void onRename(trimmed);
      }}
      onKeyDown={(event) => {
        if (event.key === 'Escape') onCancel();
      }}
    >
      <label htmlFor={`g-rename-${name}`} className="g-visually-hidden">
        Neuer Name der Kamera
      </label>
      <input
        id={`g-rename-${name}`}
        type="text"
        value={value}
        maxLength={MAX_CAMERA_NAME}
        autoComplete="off"
        autoFocus
        onChange={(event) => setValue(event.target.value)}
      />
      <button
        type="submit"
        className="g-btn g-btn--small"
        disabled={disabled || trimmed === '' || trimmed === name}
      >
        Speichern
      </button>
      <button type="button" className="g-btn g-btn--small g-btn--quiet" onClick={onCancel}>
        Abbrechen
      </button>
    </form>
  );
}

function FolderBar({
  info,
  onChoose,
  onAllow,
}: {
  info: FolderState;
  onChoose(): void;
  onAllow(): void;
}) {
  switch (info.kind) {
    case 'unsupported':
      return (
        <p className="g-folder">
          Videos landen im Ordner „Downloads“. (Für einen eigenen Ordner bitte Chrome oder Edge
          verwenden.)
        </p>
      );
    case 'none':
      return (
        <div className="g-folder g-folder--todo">
          <p>Wohin sollen die Videos? Bitte einmal einen Ordner wählen.</p>
          <button type="button" className="g-btn" onClick={onChoose}>
            Ordner wählen
          </button>
        </div>
      );
    case 'needsPermission':
      return (
        <div className="g-folder g-folder--todo">
          <p>Bitte den Ordner „{info.name}“ wieder freigeben, damit Videos dort landen.</p>
          <button type="button" className="g-btn" onClick={onAllow}>
            Ordner freigeben
          </button>
          <button type="button" className="g-btn g-btn--plain" onClick={onChoose}>
            Anderen Ordner wählen
          </button>
        </div>
      );
    case 'ready':
      return (
        <p className="g-folder">
          Videos werden im Ordner „{info.name}“ gespeichert.{' '}
          <button type="button" className="g-link" onClick={onChoose}>
            Ändern
          </button>
        </p>
      );
  }
}
