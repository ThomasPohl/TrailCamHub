/**
 * Camera settings (#368): what the hub reports from the camera's device info
 * (512) and what the gallery may change with 513. The field table mirrors the
 * hub's allowlist (components/camera_link/camera_settings.cpp); values are in
 * docs/camera-settings.md.
 *
 * Never part of it: the screen lock code (secureCode0..3, a secret), the
 * clock (the hub never writes the camera time) and fields whose meaning is
 * unknown. The hub leaves out the settings the camera does not support.
 */

import type { CameraCapability, CameraSettings } from './model';

type Kind = 'number' | 'flag' | 'language';
type Choices = 'image' | 'video' | 'language';

interface Field {
  kind: Kind;
  min?: number;
  max?: number;
  choices?: Choices;
}

const VERTEX: Field = { kind: 'number', min: 0, max: 1000 };

/** The settings a hub reports and the gallery may change, in the hub's order. */
export const SETTING_FIELDS: Readonly<Record<string, Field>> = {
  workMode: { kind: 'number', min: 0, max: 2 },
  shootMode: { kind: 'number', min: 0, max: 2 },
  picResolution: { kind: 'number', min: 0, max: 99, choices: 'image' },
  videoResolution: { kind: 'number', min: 0, max: 99, choices: 'video' },
  continuousShootCnt: { kind: 'number', min: 1, max: 5 },
  recordDuration: { kind: 'number', min: 1, max: 600 },
  recordAudio: { kind: 'number', min: 0, max: 1 },
  timelapseInterval: { kind: 'number', min: 1, max: 86_400 },
  pirSens: { kind: 'number', min: 0, max: 2 },
  pirCoolingTime: { kind: 'number', min: 0, max: 3600 },
  sidePirOnOff: { kind: 'number', min: 0, max: 1 },
  workTimeOnOff: { kind: 'number', min: 0, max: 1 },
  workTimeStartHour: { kind: 'number', min: 0, max: 23 },
  workTimeStartMin: { kind: 'number', min: 0, max: 59 },
  workTimeEndHour: { kind: 'number', min: 0, max: 23 },
  workTimeEndMin: { kind: 'number', min: 0, max: 59 },
  enableAiFilter: { kind: 'number', min: 0, max: 1 },
  detectionAreaVertex0X: VERTEX,
  detectionAreaVertex0Y: VERTEX,
  detectionAreaVertex1X: VERTEX,
  detectionAreaVertex1Y: VERTEX,
  detectionAreaVertex2X: VERTEX,
  detectionAreaVertex2Y: VERTEX,
  detectionAreaVertex3X: VERTEX,
  detectionAreaVertex3Y: VERTEX,
  infraRedLum: { kind: 'number', min: 1, max: 3 },
  osdInfoEnable: { kind: 'flag' },
  WiFiLedEnable: { kind: 'flag' },
  dateFormat: { kind: 'number', min: 0, max: 2 },
  timeFormat: { kind: 'number', min: 0, max: 2 },
  languageCode: { kind: 'language', choices: 'language' },
  micVol: { kind: 'number', min: 0, max: 100 },
  spkVol: { kind: 'number', min: 0, max: 100 },
};

const LANGUAGE = /^[A-Za-z-]{1,8}$/;

function validValue(field: Field, value: unknown): value is number | boolean | string {
  switch (field.kind) {
    case 'number':
      return (
        Number.isInteger(value) &&
        (value as number) >= (field.min ?? 0) &&
        (value as number) <= (field.max ?? 0)
      );
    case 'flag':
      return typeof value === 'boolean';
    case 'language':
      return typeof value === 'string' && LANGUAGE.test(value);
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * The settings of a check-in, only known fields with plausible values; a
 * field that does not fit is dropped. Undefined if there are none (older
 * firmware).
 */
export function parseSettingsReport(raw: unknown): CameraSettings | undefined {
  if (!isRecord(raw)) return undefined;
  const out: CameraSettings = {};
  for (const [name, field] of Object.entries(SETTING_FIELDS)) {
    const value = raw[name];
    if (validValue(field, value)) out[name] = value;
  }
  return Object.keys(out).length > 0 ? out : undefined;
}

function codes(raw: unknown): number[] {
  if (!Array.isArray(raw)) return [];
  return [...new Set(raw.filter((n): n is number => Number.isInteger(n) && n >= 0 && n <= 99))];
}

/** The capability of a check-in; undefined if the hub sent none. */
export function parseCapability(raw: unknown): CameraCapability | undefined {
  if (!isRecord(raw)) return undefined;
  const languages = Array.isArray(raw.languages)
    ? [
        ...new Set(
          raw.languages.filter((l): l is string => typeof l === 'string' && LANGUAGE.test(l)),
        ),
      ]
    : [];
  return {
    imgResolutions: codes(raw.imgResolutions),
    videoResolutions: codes(raw.videoResolutions),
    languages,
  };
}

export type SettingsChange = { ok: true; settings: CameraSettings } | { ok: false; error: string };

/**
 * Checks a change from the gallery: known fields only, each with a value of
 * its kind and range, and one the camera offers (its capability). A field the
 * camera did not report is one it does not support, so it cannot be changed
 * once the camera's settings are known.
 */
export function checkSettingsChange(
  raw: unknown,
  current: CameraSettings | undefined,
  capability: CameraCapability | undefined,
): SettingsChange {
  if (!isRecord(raw)) return { ok: false, error: 'settings must be an object' };
  const entries = Object.entries(raw);
  if (entries.length === 0) return { ok: false, error: 'settings must change at least one field' };
  const settings: CameraSettings = {};
  for (const [name, value] of entries) {
    const field = SETTING_FIELDS[name];
    if (!field) return { ok: false, error: `${name} cannot be changed` };
    if (!validValue(field, value)) return { ok: false, error: `Invalid value for ${name}` };
    if (current && !(name in current)) {
      return { ok: false, error: `The camera does not support ${name}` };
    }
    const offered =
      field.choices === 'image'
        ? capability?.imgResolutions
        : field.choices === 'video'
          ? capability?.videoResolutions
          : field.choices === 'language'
            ? capability?.languages
            : undefined;
    if (offered && offered.length > 0 && !(offered as unknown[]).includes(value)) {
      return { ok: false, error: `The camera does not offer ${name} ${String(value)}` };
    }
    settings[name] = value;
  }
  return { ok: true, settings };
}
