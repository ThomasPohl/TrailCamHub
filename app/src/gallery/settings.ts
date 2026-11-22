import type { CameraCapability, CameraSettings } from '../api/types';

/**
 * The camera settings the gallery offers (#368), worded for the end user.
 * Field names and values are those of the camera (docs/camera-settings.md);
 * a field the camera did not report (it does not support it) is not shown.
 * The detection area, the screen lock and the clock are not offered.
 */

export interface Choice {
  value: number | string;
  label: string;
}

interface SelectField {
  kind: 'select';
  name: string;
  label: string;
  hint?: string;
  /** The choices for this camera; the current value is added if missing. */
  choices(capability: CameraCapability | undefined): Choice[];
  /** Shown only while this is true for the settings in the form. */
  when?(form: CameraSettings): boolean;
}

interface SwitchField {
  kind: 'switch';
  name: string;
  label: string;
  hint?: string;
  /** `flag`: true / false; otherwise 1 / 0. */
  flag?: boolean;
}

/** A time of day from two fields (hour, minute). */
interface TimeField {
  kind: 'time';
  hour: string;
  minute: string;
  label: string;
  when?(form: CameraSettings): boolean;
}

export type SettingField = SelectField | SwitchField | TimeField;

export interface SettingGroup {
  title: string;
  fields: SettingField[];
}

const fixed =
  (...choices: Choice[]) =>
  (): Choice[] =>
    choices;

function seconds(s: number): string {
  if (s < 60) return `${s} Sekunden`;
  if (s < 3600) return s === 60 ? '1 Minute' : `${s / 60} Minuten`;
  return s === 3600 ? '1 Stunde' : `${s / 3600} Stunden`;
}

const secondsChoices = (...values: number[]) =>
  fixed(...values.map((value) => ({ value, label: seconds(value) })));

const percentChoices = fixed(
  ...[0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100].map((value) => ({ value, label: `${value} %` })),
);

/** Photo resolutions known from the app; the others by their number. */
const PHOTO: Record<number, string> = { 9: '48 Megapixel', 10: '60 Megapixel' };
/** Video resolutions in the order of the app's list (assumed, docs/camera-settings.md). */
const VIDEO: Record<number, string> = { 1: '720p', 2: '1080p', 4: '2,7K', 5: '4K' };
const LANGUAGES: Record<string, string> = {
  en: 'Englisch',
  de: 'Deutsch',
  fr: 'Französisch',
  es: 'Spanisch',
  du: 'Niederländisch',
  it: 'Italienisch',
  pl: 'Polnisch',
  cn: 'Chinesisch',
  jp: 'Japanisch',
  ru: 'Russisch',
};

export function photoLabel(code: number): string {
  return PHOTO[code] ?? `Stufe ${code}`;
}

export function videoLabel(code: number): string {
  return VIDEO[code] ?? `Stufe ${code}`;
}

const periodic = (form: CameraSettings) => form.workMode === 1 || form.workMode === 2;
const withWorkTime = (form: CameraSettings) => form.workTimeOnOff === 1;

export const SETTING_GROUPS: SettingGroup[] = [
  {
    title: 'Aufnahme',
    fields: [
      {
        kind: 'select',
        name: 'workMode',
        label: 'Auslösen',
        choices: fixed(
          { value: 0, label: 'Bei Bewegung' },
          { value: 1, label: 'In festen Abständen' },
          { value: 2, label: 'Zeitraffer' },
        ),
      },
      {
        kind: 'select',
        name: 'timelapseInterval',
        label: 'Abstand',
        choices: secondsChoices(30, 60, 300, 600, 1800, 3600, 7200, 21_600, 43_200, 86_400),
        when: periodic,
      },
      {
        kind: 'select',
        name: 'shootMode',
        label: 'Was aufnehmen',
        choices: fixed(
          { value: 0, label: 'Nur Foto' },
          { value: 1, label: 'Nur Video' },
          { value: 2, label: 'Foto und Video' },
        ),
      },
      {
        kind: 'select',
        name: 'picResolution',
        label: 'Fotoauflösung',
        choices: (capability) =>
          (capability?.imgResolutions ?? []).map((value) => ({ value, label: photoLabel(value) })),
      },
      {
        kind: 'select',
        name: 'continuousShootCnt',
        label: 'Fotos je Auslösung',
        choices: fixed(...[1, 2, 3, 4, 5].map((value) => ({ value, label: String(value) }))),
      },
      {
        kind: 'select',
        name: 'videoResolution',
        label: 'Videoauflösung',
        choices: (capability) =>
          (capability?.videoResolutions ?? []).map((value) => ({
            value,
            label: videoLabel(value),
          })),
      },
      {
        kind: 'select',
        name: 'recordDuration',
        label: 'Videolänge',
        choices: secondsChoices(5, 10, 15, 20, 30, 45, 60, 90, 120, 180),
      },
      { kind: 'switch', name: 'recordAudio', label: 'Ton aufnehmen' },
    ],
  },
  {
    title: 'Bewegungsmelder',
    fields: [
      {
        kind: 'select',
        name: 'pirSens',
        label: 'Empfindlichkeit',
        choices: fixed(
          { value: 0, label: 'Niedrig' },
          { value: 1, label: 'Mittel' },
          { value: 2, label: 'Hoch' },
        ),
      },
      {
        kind: 'select',
        name: 'pirCoolingTime',
        label: 'Pause nach einer Aufnahme',
        choices: secondsChoices(5, 10, 20, 30, 60, 120, 300, 600, 1800, 3600),
      },
      {
        kind: 'switch',
        name: 'workTimeOnOff',
        label: 'Nur zu bestimmten Zeiten aufnehmen',
      },
      {
        kind: 'time',
        hour: 'workTimeStartHour',
        minute: 'workTimeStartMin',
        label: 'Von',
        when: withWorkTime,
      },
      {
        kind: 'time',
        hour: 'workTimeEndHour',
        minute: 'workTimeEndMin',
        label: 'Bis',
        when: withWorkTime,
      },
    ],
  },
  {
    title: 'Kamera',
    fields: [
      {
        kind: 'select',
        name: 'infraRedLum',
        label: 'Nachtlicht (Infrarot)',
        choices: fixed(
          { value: 1, label: 'Schwach' },
          { value: 2, label: 'Mittel' },
          { value: 3, label: 'Stark' },
        ),
      },
      {
        kind: 'switch',
        name: 'osdInfoEnable',
        label: 'Datum und Uhrzeit im Bild',
        flag: true,
      },
      {
        kind: 'select',
        name: 'dateFormat',
        label: 'Datumsformat',
        choices: fixed(
          { value: 0, label: 'TT/MM/JJJJ' },
          { value: 1, label: 'MM/TT/JJJJ' },
          { value: 2, label: 'JJJJ/MM/TT' },
        ),
      },
      {
        kind: 'select',
        name: 'timeFormat',
        label: 'Uhrzeit',
        choices: fixed({ value: 2, label: '24 Stunden' }, { value: 1, label: '12 Stunden' }),
      },
      {
        kind: 'select',
        name: 'languageCode',
        label: 'Sprache der Kamera',
        choices: (capability) =>
          (capability?.languages ?? []).map((value) => ({
            value,
            label: LANGUAGES[value] ?? value,
          })),
      },
      { kind: 'select', name: 'micVol', label: 'Mikrofon', choices: percentChoices },
      { kind: 'select', name: 'spkVol', label: 'Lautsprecher', choices: percentChoices },
      { kind: 'switch', name: 'WiFiLedEnable', label: 'WLAN-Lämpchen', flag: true },
    ],
  },
];

/** The field names a setting field edits. */
export function fieldNames(field: SettingField): string[] {
  return field.kind === 'time' ? [field.hour, field.minute] : [field.name];
}

/** Whether the camera reported every field of `field` and it is shown for `form`. */
export function isShown(field: SettingField, form: CameraSettings): boolean {
  if (!fieldNames(field).every((name) => name in form)) return false;
  return field.kind === 'switch' || !field.when || field.when(form);
}

const UNIT_SECONDS: Record<string, true> = {
  timelapseInterval: true,
  recordDuration: true,
  pirCoolingTime: true,
};

/**
 * The choices of a select for this camera, with the current value added if
 * the list does not have it (a value set in the camera's own menu). A
 * `timeFormat` of 0 is the factory setting, 24 hours.
 */
export function choicesFor(
  field: SelectField,
  capability: CameraCapability | undefined,
  value: number | boolean | string | undefined,
): Choice[] {
  const choices = field.choices(capability);
  if (value === undefined || typeof value === 'boolean') return choices;
  if (choices.some((c) => c.value === value)) return choices;
  const label =
    field.name === 'timeFormat' && value === 0
      ? '24 Stunden'
      : typeof value === 'number' && field.name in UNIT_SECONDS
        ? seconds(value)
        : String(value);
  return [{ value, label }, ...choices];
}

/** The settings as the camera will have them: the last read plus what waits. */
export function effectiveSettings(
  settings: CameraSettings | undefined,
  pending: CameraSettings | undefined,
): CameraSettings {
  return { ...settings, ...pending };
}

/** The fields of `form` that differ from `base`: what to send. */
export function changedSettings(base: CameraSettings, form: CameraSettings): CameraSettings {
  const changed: CameraSettings = {};
  for (const [name, value] of Object.entries(form)) {
    if (base[name] !== value) changed[name] = value;
  }
  return changed;
}

/** "07:05" from hour and minute. */
export function timeText(hour: unknown, minute: unknown): string {
  const two = (n: unknown) => String(typeof n === 'number' ? n : 0).padStart(2, '0');
  return `${two(hour)}:${two(minute)}`;
}

/** Hour and minute of "HH:MM"; undefined if it is not a time. */
export function parseTime(text: string): { hour: number; minute: number } | undefined {
  const match = /^(\d{1,2}):(\d{2})$/.exec(text);
  if (!match) return undefined;
  const hour = Number(match[1]);
  const minute = Number(match[2]);
  return hour <= 23 && minute <= 59 ? { hour, minute } : undefined;
}

/** The hub's reason in words for the end user (German); the original as a fallback. */
export function settingsErrorText(error: string): string {
  if (/does not (support|offer)/.test(error)) {
    return 'Die Kamera kann diese Einstellung nicht übernehmen.';
  }
  if (/did not answer|not be sent|waited too long|interrupted/.test(error)) {
    return 'Die Kamera war nicht erreichbar. Bitte noch einmal speichern.';
  }
  return `Die Kamera hat die Änderung nicht übernommen (${error}).`;
}
