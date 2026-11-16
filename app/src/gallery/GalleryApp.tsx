import { useCallback, useEffect, useRef, useState } from 'react';
import { ApiError } from '../api/http';
import * as realFolder from './folder';
import type { FolderState } from './folder';
import { cameraTime, clockTime, folderName, stateText } from './text';
import { MAX_CAMERA_NAME, type GalleryApi, type GalleryStatus, type Recording } from './types';

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

interface DeleteQuestion {
  question: string;
  mediaIds: number[];
}

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
  const [selected, setSelected] = useState<Set<number>>(() => new Set());
  const [busy, setBusy] = useState(false);
  const saving = useRef(false);

  const reload = useCallback(async () => {
    try {
      const s = await api.status();
      setStatus(s);
      const id = camId && s.cameras.some((c) => c.camId === camId) ? camId : s.cameras[0]?.camId;
      setCamId(id);
      setItems(id ? await api.recordings(id) : []);
      setLoadError(undefined);
    } catch (error) {
      setLoadError(error instanceof ApiError && error.status === 401 ? 'signin' : errorText(error));
    }
  }, [api, camId]);

  useEffect(() => {
    void reload();
    if (pollMs <= 0) return;
    const timer = setInterval(() => void reload(), pollMs);
    return () => clearInterval(timer);
  }, [reload, pollMs]);

  useEffect(() => {
    folder.folderState().then(setFolderInfo, () => setFolderInfo({ kind: 'unsupported' }));
  }, [folder]);

  const cameraName = status?.cameras.find((c) => c.camId === camId)?.name ?? 'Kamera';

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
    void (async () => {
      const done: number[] = [];
      try {
        for (const r of pending) {
          const data = await api.download(r);
          await folder.saveToFolder(folderName(cameraName), r.fileName, data);
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
  }, [items, folderInfo, saved, api, folder, cameraName, markSaved]);

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
        await folder.saveToFolder(folderName(cameraName), r.fileName, data);
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
  const renameCamera = async (id: string, name: string): Promise<boolean> => {
    setBusy(true);
    try {
      const stored = await api.renameCamera(id, name);
      setNotice(
        `Die Kamera heißt jetzt „${stored}“. Neue Videos landen im Unterordner „${folderName(stored)}“.`,
      );
      await reload();
      return true;
    } catch (error) {
      setNotice(`Umbenennen hat nicht geklappt: ${errorText(error)}`);
      return false;
    } finally {
      setBusy(false);
    }
  };

  const pickFolder = async (allow: boolean) => {
    try {
      setFolderInfo(await (allow ? folder.allowFolder() : folder.chooseFolder()));
    } catch {
      // The user closed the dialog; nothing to report.
    }
  };

  const toggle = (mediaId: number) =>
    setSelected((current) => {
      const next = new Set(current);
      if (!next.delete(mediaId)) next.add(mediaId);
      return next;
    });

  const t = now();
  const newOnes = items?.filter((r) => r.state === 'new') ?? [];
  const chosen = items?.filter((r) => selected.has(r.mediaId) && selectable(r)) ?? [];
  const chosenVideos = chosen.filter(videoWanted);

  return (
    <div className="gallery">
      <header className="g-header">
        <h1>Wildkamera</h1>
        {status && (
          <p className="g-status">
            {status.lastContact > 0
              ? `Letzter Kontakt zum Hub: ${clockTime(status.lastContact, t)}`
              : 'Der Hub hat sich noch nicht gemeldet.'}
            {status.nextContact !== undefined &&
              ` · Nächster Kontakt: gegen ${clockTime(status.nextContact, t)}`}
            {status.hubBatPercent !== undefined && ` · Akku ${status.hubBatPercent} %`}
          </p>
        )}
      </header>

      {api.isDemo && (
        <p className="g-banner" role="note">
          Vorführung: Beispielbilder, kein echter Hub. Der Hub „meldet sich“ hier alle 20 Sekunden.
        </p>
      )}

      <FolderBar
        info={folderInfo}
        onChoose={() => void pickFolder(false)}
        onAllow={() => void pickFolder(true)}
      />

      {notice && (
        <p className="g-notice" role="status">
          {notice}
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

      {status && status.cameras.length > 1 && (
        <nav className="g-tabs" aria-label="Kameras">
          {status.cameras.map((c) => (
            <button
              key={c.camId}
              type="button"
              className={c.camId === camId ? 'g-tab g-tab--on' : 'g-tab'}
              aria-pressed={c.camId === camId}
              onClick={() => {
                setCamId(c.camId);
                setItems(undefined);
                setSelected(new Set());
              }}
            >
              {c.name}
            </button>
          ))}
        </nav>
      )}

      {status && camId && (
        <CameraHeading
          key={camId}
          name={cameraName}
          disabled={busy}
          onRename={(name) => renameCamera(camId, name)}
        />
      )}

      {items && newOnes.length > 0 && (
        <div className="g-toolbar">
          <button
            type="button"
            className="g-btn g-btn--danger"
            disabled={busy}
            onClick={() =>
              setConfirm({
                question:
                  newOnes.length === 1
                    ? '1 neue Aufnahme löschen?'
                    : `Alle ${newOnes.length} neuen Aufnahmen löschen?`,
                mediaIds: newOnes.flatMap((r) => r.mediaIds),
              })
            }
          >
            Alle neuen löschen ({newOnes.length})
          </button>
        </div>
      )}

      {camId && chosen.length > 0 && (
        <div className="g-selbar" role="region" aria-label="Auswahl">
          <p>
            {chosen.length === 1
              ? '1 Aufnahme ausgewählt'
              : `${chosen.length} Aufnahmen ausgewählt`}
          </p>
          <div className="g-card__actions">
            {chosenVideos.length > 0 && (
              <button
                type="button"
                className="g-btn"
                disabled={busy}
                onClick={() =>
                  void act(async () => {
                    await api.requestVideos(
                      camId,
                      chosenVideos.map((r) => r.mediaId),
                    );
                    setSelected(new Set());
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
                  mediaIds: chosen.flatMap((r) => r.mediaIds),
                })
              }
            >
              Löschen ({chosen.length})
            </button>
            <button
              type="button"
              className="g-btn g-btn--plain"
              onClick={() => setSelected(new Set())}
            >
              Auswahl aufheben
            </button>
          </div>
        </div>
      )}

      {items === undefined && !loadError && <p className="g-hint">Bilder werden geladen …</p>}
      {items && items.length === 0 && (
        <p className="g-hint">Keine Aufnahmen. Neue Bilder erscheinen nach dem nächsten Kontakt.</p>
      )}

      {items && items.length > 0 && camId && (
        <ul className="g-grid" aria-label="Aufnahmen">
          {items.map((r) => (
            <li
              key={r.mediaId}
              className={`g-card g-card--${r.state}${chosen.includes(r) ? ' g-card--chosen' : ''}`}
            >
              {selectable(r) && (
                <label className="g-card__select">
                  <input
                    type="checkbox"
                    checked={selected.has(r.mediaId)}
                    onChange={() => toggle(r.mediaId)}
                  />
                  Auswählen
                </label>
              )}
              <img
                src={r.thumbUrl}
                alt={`Aufnahme ${r.cameraFiles.join(', ')} vom ${cameraTime(r.mediaTime)}`}
              />
              <p className="g-card__time">{cameraTime(r.mediaTime)}</p>
              <p className="g-card__files" title="Dateien auf der Kamera">
                {r.cameraFiles.map((name) => (
                  <span key={name}>{name}</span>
                ))}
              </p>
              <p className="g-card__state">
                {stateText(r.state, saved.has(r.mediaId), status?.nextContact, t)}
                {r.state === 'failed' && r.failReason && ` – ${r.failReason}`}
              </p>
              <div className="g-card__actions">
                {(r.state === 'new' || r.state === 'failed') && (
                  <>
                    {r.fileType === 2 && (
                      <button
                        type="button"
                        className="g-btn"
                        disabled={busy}
                        onClick={() => void act(() => api.requestVideos(camId, [r.mediaId]))}
                      >
                        Video holen
                      </button>
                    )}
                    <button
                      type="button"
                      className="g-btn g-btn--danger"
                      disabled={busy}
                      onClick={() => void act(() => api.requestDelete(camId, r.mediaIds))}
                    >
                      Löschen
                    </button>
                  </>
                )}
                {(r.state === 'videoRequested' || r.state === 'deleteRequested') && (
                  <button
                    type="button"
                    className="g-btn g-btn--plain"
                    disabled={busy}
                    onClick={() => void act(() => api.withdraw(camId, r))}
                  >
                    Zurücknehmen
                  </button>
                )}
                {r.state === 'videoReady' && (
                  <>
                    <button type="button" className="g-btn" onClick={() => void saveOne(r)}>
                      {saved.has(r.mediaId) ? 'Nochmal speichern' : 'Video speichern'}
                    </button>
                    <button
                      type="button"
                      className="g-btn g-btn--danger"
                      disabled={busy}
                      onClick={() => void act(() => api.requestDelete(camId, r.mediaIds))}
                    >
                      Löschen
                    </button>
                  </>
                )}
              </div>
            </li>
          ))}
        </ul>
      )}

      {confirm && camId && (
        <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-confirm">
          <div className="g-dialog__box">
            <p id="g-confirm">{confirm.question}</p>
            <p className="g-hint">
              Bis zum nächsten Kontakt mit dem Hub lässt sich das noch zurücknehmen.
            </p>
            <div className="g-card__actions">
              <button
                type="button"
                className="g-btn g-btn--danger"
                onClick={() => {
                  setConfirm(undefined);
                  setSelected(new Set());
                  void act(
                    () => api.requestDelete(camId, confirm.mediaIds),
                    'Die Aufnahmen werden beim nächsten Kontakt gelöscht.',
                  );
                }}
              >
                Ja, löschen
              </button>
              <button
                type="button"
                className="g-btn g-btn--plain"
                onClick={() => setConfirm(undefined)}
              >
                Abbrechen
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

/** The camera's name as heading, with a form to rename it (#273). */
function CameraHeading({
  name,
  disabled,
  onRename,
}: {
  name: string;
  disabled: boolean;
  onRename(name: string): Promise<boolean>;
}) {
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(name);
  const trimmed = value.trim();

  if (!editing) {
    return (
      <div className="g-camera-row">
        <h2 className="g-camera">Kamera „{name}“</h2>
        <button
          type="button"
          className="g-btn g-btn--plain"
          disabled={disabled}
          onClick={() => {
            setValue(name);
            setEditing(true);
          }}
        >
          Umbenennen
        </button>
      </div>
    );
  }
  return (
    <form
      className="g-camera-row g-rename"
      onSubmit={(event) => {
        event.preventDefault();
        void onRename(trimmed).then((ok) => {
          if (ok) setEditing(false);
        });
      }}
    >
      <label htmlFor="g-rename-name">Neuer Name der Kamera</label>
      <input
        id="g-rename-name"
        type="text"
        value={value}
        maxLength={MAX_CAMERA_NAME}
        autoComplete="off"
        autoFocus
        onChange={(event) => setValue(event.target.value)}
      />
      <button
        type="submit"
        className="g-btn"
        disabled={disabled || trimmed === '' || trimmed === name}
      >
        Speichern
      </button>
      <button type="button" className="g-btn g-btn--plain" onClick={() => setEditing(false)}>
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
