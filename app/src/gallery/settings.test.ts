import { describe, expect, it } from 'vitest';
import {
  changedSettings,
  choicesFor,
  effectiveSettings,
  isShown,
  parseTime,
  photoLabel,
  SETTING_GROUPS,
  settingsErrorText,
  timeText,
  videoLabel,
  type SettingField,
} from './settings';

const all = SETTING_GROUPS.flatMap((g) => g.fields);
const byName = (name: string): SettingField => {
  const field = all.find((f) => f.kind !== 'time' && f.name === name);
  if (!field) throw new Error(name);
  return field;
};
const select = (name: string) => {
  const field = byName(name);
  if (field.kind !== 'select') throw new Error(name);
  return field;
};

describe('camera settings in the gallery (#368)', () => {
  it('sends only the fields that differ', () => {
    const base = { workMode: 0, pirSens: 2, osdInfoEnable: true };
    expect(changedSettings(base, { ...base })).toEqual({});
    expect(changedSettings(base, { ...base, pirSens: 1, osdInfoEnable: false })).toEqual({
      pirSens: 1,
      osdInfoEnable: false,
    });
  });

  it('shows a waiting change over the last read', () => {
    expect(effectiveSettings({ pirSens: 2, workMode: 0 }, { pirSens: 0 })).toEqual({
      pirSens: 0,
      workMode: 0,
    });
    expect(effectiveSettings(undefined, undefined)).toEqual({});
  });

  it('shows only fields the camera reported, some only when they apply', () => {
    expect(isShown(byName('spkVol'), { micVol: 50 })).toBe(false);
    expect(isShown(byName('micVol'), { micVol: 50 })).toBe(true);
    const interval = byName('timelapseInterval');
    expect(isShown(interval, { workMode: 0, timelapseInterval: 60 })).toBe(false);
    expect(isShown(interval, { workMode: 1, timelapseInterval: 60 })).toBe(true);
    const from = all.find((f) => f.kind === 'time' && f.label === 'Von');
    if (!from) throw new Error('Von');
    const hours = { workTimeStartHour: 18, workTimeStartMin: 0 };
    expect(isShown(from, { ...hours, workTimeOnOff: 0 })).toBe(false);
    expect(isShown(from, { ...hours, workTimeOnOff: 1 })).toBe(true);
    expect(isShown(from, { workTimeOnOff: 1 })).toBe(false);
  });

  it('takes the resolutions and languages from the camera', () => {
    const capability = { imgResolutions: [6, 10], videoResolutions: [1, 4], languages: ['de'] };
    expect(choicesFor(select('picResolution'), capability, 10)).toEqual([
      { value: 6, label: 'Stufe 6' },
      { value: 10, label: '60 Megapixel' },
    ]);
    expect(choicesFor(select('videoResolution'), capability, 4).map((c) => c.label)).toEqual([
      '720p',
      '2,7K',
    ]);
    expect(choicesFor(select('languageCode'), capability, 'de')).toEqual([
      { value: 'de', label: 'Deutsch' },
    ]);
    expect(photoLabel(9)).toBe('48 Megapixel');
    expect(videoLabel(3)).toBe('Stufe 3');
  });

  it('keeps a value the list does not have', () => {
    expect(choicesFor(select('pirCoolingTime'), undefined, 45)[0]).toEqual({
      value: 45,
      label: '45 Sekunden',
    });
    expect(choicesFor(select('timeFormat'), undefined, 0)[0]).toEqual({
      value: 0,
      label: '24 Stunden',
    });
    expect(choicesFor(select('timelapseInterval'), undefined, 3600)).toHaveLength(10);
  });

  it('reads and writes times of day', () => {
    expect(timeText(7, 5)).toBe('07:05');
    expect(timeText(undefined, undefined)).toBe('00:00');
    expect(parseTime('18:00')).toEqual({ hour: 18, minute: 0 });
    expect(parseTime('24:00')).toBeUndefined();
    expect(parseTime('')).toBeUndefined();
  });

  it('words the hub reasons for the end user', () => {
    expect(settingsErrorText('the camera does not support spkVol')).toMatch(/nicht übernehmen/);
    expect(
      settingsErrorText('the camera did not answer within 15 s; it may have been done'),
    ).toMatch(/nicht erreichbar/);
    expect(settingsErrorText('the camera answered result -1')).toMatch(/result -1/);
  });
});
