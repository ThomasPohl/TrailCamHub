import { useState } from 'react';
import type { DeleteTarget } from '../api/types';
import type { GalleryApi, GalleryStatus, Recording } from './types';

type Hub = NonNullable<GalleryStatus['hub']>;

const ALL_TARGETS: DeleteTarget[] = ['camera', 'hub', 'backend'];

const LABELS: Record<DeleteTarget, string> = {
  camera: 'Kamera',
  hub: 'Hub',
  backend: 'Galerie',
};

const EXPLAIN: Record<DeleteTarget, string> = {
  camera: 'Die Dateien auf der Speicherkarte der Kamera.',
  hub: 'Die Kopien auf der Speicherkarte des Hubs (Vorschaubilder und geholte Videos).',
  backend: 'Der Eintrag hier in der Galerie samt Vorschaubild und Video.',
};

/** "Kamera, Hub, Galerie": the targets in a fixed order, in words. */
export const targetNames = (targets: DeleteTarget[]): string =>
  ALL_TARGETS.filter((t) => targets.includes(t))
    .map((t) => LABELS[t])
    .join(', ');

/**
 * Whether a delete on the camera loses a video: the recording has one, and
 * the gallery does not have it yet (#461). Only a video that is ready in the
 * gallery is safe.
 */
export const losesVideo = (r: Recording): boolean => r.fileType === 2 && r.state !== 'videoReady';

interface PickerProps {
  hub: Hub;
  value: DeleteTarget[];
  onChange(targets: DeleteTarget[]): void;
  disabled?: boolean;
}

/** Three checkboxes; an old hub firmware cannot delete on the hub alone (#461). */
export function DeleteTargetsPicker({ hub, value, onChange, disabled }: PickerProps) {
  const toggle = (target: DeleteTarget) => {
    onChange(ALL_TARGETS.filter((t) => (t === target ? !value.includes(t) : value.includes(t))));
  };
  return (
    <fieldset className="g-targets" disabled={disabled}>
      <legend className="g-sr">Wo gelöscht wird</legend>
      {ALL_TARGETS.map((target) => {
        const unavailable = target === 'hub' && !hub.deletesByTarget;
        return (
          <div key={target} className="g-targets__row">
            <label>
              <input
                type="checkbox"
                checked={value.includes(target)}
                disabled={unavailable}
                onChange={() => toggle(target)}
              />{' '}
              {LABELS[target]}
            </label>
            <span className="g-hint">{EXPLAIN[target]}</span>
          </div>
        );
      })}
      {!hub.deletesByTarget && (
        <p className="g-hint">
          Der Hub löscht bisher nur auf der Kamera und in der Galerie. Für den Hub als Ziel braucht
          er die neue Firmware.
        </p>
      )}
      {value.length === 0 && (
        <p className="g-error" role="alert">
          Mindestens ein Ziel muss gewählt sein.
        </p>
      )}
    </fieldset>
  );
}

interface DeleteDialogProps {
  question: string;
  items: Recording[];
  hub: Hub | undefined;
  onCancel(): void;
  /** `targets` is what the user sees in the dialog, also when he did not change it. */
  onConfirm(targets: DeleteTarget[]): void;
}

/** What a delete does, where, and what is lost with it (#461). */
export function DeleteDialog({ question, items, hub, onCancel, onConfirm }: DeleteDialogProps) {
  const [targets, setTargets] = useState<DeleteTarget[]>(
    hub?.deleteTargets ?? ['camera', 'backend'],
  );
  const [changing, setChanging] = useState(false);
  const lost = targets.includes('camera') ? items.filter(losesVideo).length : 0;
  return (
    <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-confirm">
      <div className="g-dialog__box">
        <p id="g-confirm">{question}</p>
        <p>
          Wird gelöscht auf: <strong>{targetNames(targets) || '–'}</strong>
          {hub && (
            <>
              {' '}
              <button
                type="button"
                className="g-btn g-btn--quiet"
                aria-expanded={changing}
                onClick={() => setChanging((on) => !on)}
              >
                Ändern
              </button>
            </>
          )}
        </p>
        {hub && changing && <DeleteTargetsPicker hub={hub} value={targets} onChange={setTargets} />}
        {lost > 0 && (
          <p className="g-error" role="alert">
            {lost === 1
              ? 'Das Video ist noch nicht in der Galerie und geht verloren.'
              : `${lost} Videos sind noch nicht in der Galerie und gehen verloren.`}
          </p>
        )}
        {targets.includes('backend') && (
          <p className="g-hint">
            Bis zum nächsten Kontakt mit dem Hub lässt sich das noch zurücknehmen.
          </p>
        )}
        <div className="g-dialog__actions">
          <button type="button" className="g-btn g-btn--quiet" onClick={onCancel}>
            Abbrechen
          </button>
          <button
            type="button"
            className="g-btn g-btn--danger"
            disabled={targets.length === 0}
            onClick={() => onConfirm(targets)}
          >
            {lost > 0 ? 'Trotzdem löschen' : 'Ja, löschen'}
          </button>
        </div>
      </div>
    </div>
  );
}

interface SettingsProps {
  api: Pick<GalleryApi, 'setDeleteTargets'>;
  hub: Hub;
  onSaved(): void;
  onClose(): void;
}

/** The hub's setting: where a delete applies by default, for every user (#461). */
export function DeleteTargetsDialog({ api, hub, onSaved, onClose }: SettingsProps) {
  const [targets, setTargets] = useState<DeleteTarget[]>(hub.deleteTargets);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const save = async () => {
    setBusy(true);
    setError(undefined);
    try {
      await api.setDeleteTargets(hub.hubId, targets);
      onSaved();
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      setBusy(false);
    }
  };
  return (
    <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-targets">
      <div className="g-dialog__box">
        <p id="g-targets">Wo soll „Löschen“ wirken?</p>
        <p className="g-hint">
          Gilt für diesen Hub und alle, die ihn nutzen. Beim einzelnen Löschen lässt es sich ändern.
          Normalerweise bleiben die Dateien auf der Kamera.
        </p>
        <DeleteTargetsPicker hub={hub} value={targets} onChange={setTargets} disabled={busy} />
        {targets.includes('camera') && (
          <p className="g-hint">
            Löschen auf der Kamera lässt sich nicht rückgängig machen. Videos, die noch nicht in der
            Galerie sind, gehen dann verloren; die App warnt davor.
          </p>
        )}
        {error && (
          <p className="g-error" role="alert">
            {error}
          </p>
        )}
        <div className="g-dialog__actions">
          <button type="button" className="g-btn g-btn--quiet" onClick={onClose}>
            Abbrechen
          </button>
          <button
            type="button"
            className="g-btn"
            disabled={busy || targets.length === 0}
            onClick={() => void save()}
          >
            Speichern
          </button>
        </div>
      </div>
    </div>
  );
}
