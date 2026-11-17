import { describe, expect, it } from 'vitest';
import {
  cameraFile,
  cameraHints,
  cameraTime,
  durationText,
  faultText,
  folderName,
  videoFileName,
} from './text';

// 2026-11-15 14:02 on the camera's wall clock (read as UTC, docs/clock.md).
const WALL = Date.UTC(2026, 10, 15, 14, 2) / 1000;

describe('gallery texts', () => {
  it('shows the camera wall clock unchanged', () => {
    expect(cameraTime(WALL)).toBe('15.11. 14:02');
  });

  it('builds a file name from camera name and wall clock', () => {
    expect(videoFileName('Garten', WALL, 36)).toBe('Garten_2026-11-15_14-02_DSCF0036.mp4');
    expect(videoFileName('Am Teich/Süd', WALL, 7)).toBe(
      'Am-Teich-Süd_2026-11-15_14-02_DSCF0007.mp4',
    );
  });

  it('makes a folder name Windows accepts', () => {
    expect(folderName('Garten')).toBe('Garten');
    expect(folderName('Am Teich: Süd/Ost?')).toBe('Am Teich- Süd-Ost-');
    expect(folderName('Wiese...')).toBe('Wiese');
    expect(folderName('..')).toBe('Kamera');
  });

  it("names the camera's files as on its SD card", () => {
    expect(cameraFile(11, 0)).toBe('DSCF0011.JPG');
    expect(cameraFile(12, 2)).toBe('DSCF0012.AVI');
  });

  it('words the faults for the admin', () => {
    const now = WALL;
    expect(durationText(45 * 60)).toBe('45 min');
    expect(durationText(5 * 3600)).toBe('5 h');
    expect(durationText(3 * 86400)).toBe('3 Tage');
    expect(faultText({ kind: 'hubBatteryLow', level: 'warning', value: 20 }, undefined, now)).toBe(
      'Hub-Akku schwach (20 %).',
    );
    expect(
      faultText({ kind: 'sdFull', level: 'error', camId: 'C1', value: 150 }, 'Wiese', now),
    ).toBe('Kamera „Wiese“: SD-Karte voll (150 MB frei).');
    expect(
      faultText(
        { kind: 'requestsFailed', level: 'warning', camId: 'C1', value: 2, detail: 'too large' },
        undefined,
        now,
      ),
    ).toBe('Kamera „C1“: 2 Aufträge fehlgeschlagen – too large.');
    expect(
      faultText(
        { kind: 'firmwareDowngrade', level: 'warning', detail: 'v0.1.13 → v0.1.12' },
        undefined,
        now,
      ),
    ).toBe('Der Hub läuft wieder mit älterer Firmware (v0.1.13 → v0.1.12) – Update zurückgerollt?');
  });

  it('words what needs a look at the camera (#304)', () => {
    const base = { camId: 'C1', name: 'Wiese', lastRead: 1000, sdCard: 'ok' as const };
    expect(cameraHints({ ...base, batPercent: 80, sdFreeMB: 20_000 }, 2000)).toEqual([]);
    expect(cameraHints({ ...base, batPercent: 8, sdFreeMB: 150 }, 2000)).toEqual([
      'Der Akku der Kamera ist fast leer (8 %). Bitte bald laden oder tauschen.',
      'Die SD-Karte der Kamera ist voll. Bitte alte Aufnahmen löschen.',
    ]);
    expect(cameraHints({ ...base, sdCard: 'missing' }, 1000 + 2 * 86400)).toEqual([
      'In der Kamera steckt keine SD-Karte, oder sie ist defekt.',
      'Die Kamera wurde seit über einem Tag nicht gelesen. Bitte die Kamera prüfen.',
    ]);
    // Never read: a 0 % battery is no measurement.
    expect(cameraHints({ camId: 'C2', name: 'Neu', batPercent: 0, lastRead: 0 }, 2000)).toEqual([]);
  });
});
