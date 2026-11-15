import { useCallback, useEffect, useRef, useState } from 'react';
import { ApiError } from '../api/http';
import * as realFolder from './folder';
import type { FolderState } from './folder';
import { cameraTime, clockTime, stateText } from './text';
import type { GalleryApi, GalleryStatus, Recording } from './types';

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

export function GalleryApp({ api, folder = realFolder, now = unixNow, pollMs = 15_000 }: Props) {
  const [status, setStatus] = useState<GalleryStatus>();
  const [camId, setCamId] = useState<string>();
  const [items, setItems] = useState<Recording[]>();
  const [loadError, setLoadError] = useState<string>();
  const [folderInfo, setFolderInfo] = useState<FolderState>({ kind: 'none' });
  const [saved, setSaved] = useState<Set<number>>(loadSaved);
  const [notice, setNotice] = useState<string>();
  const [confirmAll, setConfirmAll] = useState(false);
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
          await folder.saveToFolder(cameraName, r.fileName, data);
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
        await folder.saveToFolder(cameraName, r.fileName, data);
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

  const pickFolder = async (allow: boolean) => {
    try {
      setFolderInfo(await (allow ? folder.allowFolder() : folder.chooseFolder()));
    } catch {
      // The user closed the dialog; nothing to report.
    }
  };

  const t = now();
  const newOnes = items?.filter((r) => r.state === 'new') ?? [];

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
              }}
            >
              {c.name}
            </button>
          ))}
        </nav>
      )}

      {items && newOnes.length > 0 && (
        <div className="g-toolbar">
          <button
            type="button"
            className="g-btn g-btn--danger"
            disabled={busy}
            onClick={() => setConfirmAll(true)}
          >
            Alle neuen löschen ({newOnes.length})
          </button>
        </div>
      )}

      {items === undefined && !loadError && <p className="g-hint">Bilder werden geladen …</p>}
      {items && items.length === 0 && (
        <p className="g-hint">Keine Aufnahmen. Neue Bilder erscheinen nach dem nächsten Kontakt.</p>
      )}

      {items && items.length > 0 && camId && (
        <ul className="g-grid" aria-label="Aufnahmen">
          {items.map((r) => (
            <li key={r.mediaId} className={`g-card g-card--${r.state}`}>
              <img src={r.thumbUrl} alt={`Aufnahme vom ${cameraTime(r.mediaTime)}`} />
              <p className="g-card__time">{cameraTime(r.mediaTime)}</p>
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
                      onClick={() => void act(() => api.requestDelete(camId, [r.mediaId]))}
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
                    onClick={() => void act(() => api.withdraw(camId, r.mediaId))}
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
                      onClick={() => void act(() => api.requestDelete(camId, [r.mediaId]))}
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

      {confirmAll && camId && (
        <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-confirm">
          <div className="g-dialog__box">
            <p id="g-confirm">
              {newOnes.length === 1
                ? '1 neue Aufnahme löschen?'
                : `Alle ${newOnes.length} neuen Aufnahmen löschen?`}
            </p>
            <p className="g-hint">
              Bis zum nächsten Kontakt mit dem Hub lässt sich das noch zurücknehmen.
            </p>
            <div className="g-card__actions">
              <button
                type="button"
                className="g-btn g-btn--danger"
                onClick={() => {
                  setConfirmAll(false);
                  void act(
                    () =>
                      api.requestDelete(
                        camId,
                        newOnes.map((r) => r.mediaId),
                      ),
                    'Die Aufnahmen werden beim nächsten Kontakt gelöscht.',
                  );
                }}
              >
                Ja, löschen
              </button>
              <button
                type="button"
                className="g-btn g-btn--plain"
                onClick={() => setConfirmAll(false)}
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
