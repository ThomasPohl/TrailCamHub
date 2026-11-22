import { useMemo, useState } from 'react';
import type { CameraSettings } from '../api/types';
import type { GalleryCamera } from './types';
import {
  changedSettings,
  choicesFor,
  effectiveSettings,
  fieldNames,
  isShown,
  parseTime,
  SETTING_GROUPS,
  settingsErrorText,
  timeText,
  type SettingField,
} from './settings';

interface Props {
  camera: GalleryCamera;
  busy: boolean;
  /** Sends the changed fields; true when they are stored for the hub. */
  onSave(changed: CameraSettings): Promise<boolean>;
  /** Takes back the change that waits for the hub. */
  onWithdraw(): void;
  onClose(): void;
}

/**
 * The camera's settings (#368). Shows the values as the camera will have
 * them (last read plus a change that waits) and sends only what the user
 * changed; the hub passes it on at its next contact.
 */
export function SettingsDialog({ camera, busy, onSave, onWithdraw, onClose }: Props) {
  const base = useMemo(
    () => effectiveSettings(camera.settings, camera.settingsPending),
    [camera.settings, camera.settingsPending],
  );
  const [form, setForm] = useState<CameraSettings>(base);
  // New values from the hub, or a change taken back: the form starts again
  // from them. Compared by value, as every poll brings new objects.
  const baseKey = JSON.stringify(base);
  const [shownBase, setShownBase] = useState(baseKey);
  if (shownBase !== baseKey) {
    setShownBase(baseKey);
    setForm(base);
  }
  const changed = changedSettings(base, form);
  const count = Object.keys(changed).length;
  const set = (values: CameraSettings) => setForm((f) => ({ ...f, ...values }));

  const field = (f: SettingField) => {
    if (!isShown(f, form)) return null;
    const id = `g-set-${fieldNames(f).join('-')}`;
    if (f.kind === 'switch') {
      const value = form[f.name];
      return (
        <div key={id} className="g-settings__row g-settings__row--switch">
          <input
            id={id}
            type="checkbox"
            checked={value === true || value === 1}
            disabled={busy}
            onChange={(e) =>
              set({ [f.name]: f.flag ? e.target.checked : e.target.checked ? 1 : 0 })
            }
          />
          <label htmlFor={id}>{f.label}</label>
        </div>
      );
    }
    if (f.kind === 'time') {
      return (
        <div key={id} className="g-settings__row">
          <label htmlFor={id}>{f.label}</label>
          <input
            id={id}
            type="time"
            value={timeText(form[f.hour], form[f.minute])}
            disabled={busy}
            onChange={(e) => {
              const time = parseTime(e.target.value);
              if (time) set({ [f.hour]: time.hour, [f.minute]: time.minute });
            }}
          />
        </div>
      );
    }
    const value = form[f.name];
    const choices = choicesFor(f, camera.capability, value);
    return (
      <div key={id} className="g-settings__row">
        <label htmlFor={id}>{f.label}</label>
        <select
          id={id}
          value={String(value)}
          disabled={busy}
          onChange={(e) => {
            const choice = choices.find((c) => String(c.value) === e.target.value);
            if (choice) set({ [f.name]: choice.value });
          }}
        >
          {choices.map((c) => (
            <option key={String(c.value)} value={String(c.value)}>
              {c.label}
            </option>
          ))}
        </select>
      </div>
    );
  };

  const groups = SETTING_GROUPS.map((group) => ({
    title: group.title,
    rows: group.fields.map(field).filter(Boolean),
  })).filter((g) => g.rows.length > 0);

  return (
    <div className="g-dialog" role="dialog" aria-modal="true" aria-labelledby="g-settings">
      <div className="g-dialog__box g-settings">
        <p id="g-settings" className="g-settings__title">
          Einstellungen der Kamera „{camera.name}“
        </p>
        {camera.settingsState === 'requested' && (
          <p className="g-hint" role="status">
            Eine Änderung wartet auf den nächsten Kontakt mit dem Hub.{' '}
            <button type="button" className="g-link" disabled={busy} onClick={onWithdraw}>
              Zurücknehmen
            </button>
          </p>
        )}
        {camera.settingsState === 'delivered' && (
          <p className="g-hint" role="status">
            Der Hub hat die Änderung. Die Kamera übernimmt sie, sobald der Hub sie das nächste Mal
            abfragt.
          </p>
        )}
        {camera.settingsError && !camera.settingsState && (
          <p className="g-error" role="alert">
            {settingsErrorText(camera.settingsError)}
          </p>
        )}
        {groups.length === 0 ? (
          <p className="g-hint">
            Der Hub hat die Einstellungen dieser Kamera noch nicht gemeldet. Sie erscheinen nach der
            nächsten Abfrage der Kamera.
          </p>
        ) : (
          groups.map((g) => (
            <fieldset key={g.title} className="g-settings__group">
              <legend>{g.title}</legend>
              {g.rows}
            </fieldset>
          ))
        )}
        <p className="g-hint">
          Änderungen gehen beim nächsten Kontakt an den Hub und von dort an die Kamera.
        </p>
        <div className="g-dialog__actions">
          <button type="button" className="g-btn g-btn--quiet" onClick={onClose}>
            {count > 0 ? 'Abbrechen' : 'Schließen'}
          </button>
          <button
            type="button"
            className="g-btn"
            disabled={busy || count === 0}
            onClick={() => {
              void onSave(changed).then((ok) => {
                if (ok) onClose();
              });
            }}
          >
            Speichern
          </button>
        </div>
      </div>
    </div>
  );
}
