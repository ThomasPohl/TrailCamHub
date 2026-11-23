import { describe, expect, it } from 'vitest';
import {
  badgeText,
  cameraFile,
  cameraHints,
  cameraTime,
  durationText,
  faultText,
  folderName,
  memoryText,
  skipReasonText,
  uplinkText,
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
    expect(
      faultText(
        {
          kind: 'firmwareUpdateFailed',
          level: 'warning',
          value: 1,
          detail: 'v0.1.13 → v0.1.14: no check-in',
        },
        undefined,
        now,
      ),
    ).toBe('Firmware-Update zurückgerollt (v0.1.13 → v0.1.14: no check-in)');
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

describe('hub connection (#149)', () => {
  it('names the transport and rates the modem signal', () => {
    expect(uplinkText('wifi')).toBe('WLAN');
    expect(uplinkText('ethernet', -60)).toBe('Ethernet');
    expect(uplinkText('modem')).toBe('Mobilfunk');
    expect(uplinkText('modem', -80)).toBe('Mobilfunk, Signal -80 dBm (gut)');
    expect(uplinkText('modem', -91)).toBe('Mobilfunk, Signal -91 dBm (mittel)');
    expect(uplinkText('modem', -110)).toBe('Mobilfunk, Signal -110 dBm (sehr schwach)');
  });

  it('adds the mobile network when the hub reports it', () => {
    expect(uplinkText('modem', undefined, 'Telekom.de')).toBe('Mobilfunk (Telekom.de)');
    expect(uplinkText('modem', -91, 'Telekom.de')).toBe(
      'Mobilfunk (Telekom.de), Signal -91 dBm (mittel)',
    );
    // Only over the modem.
    expect(uplinkText('wifi', undefined, 'Telekom.de')).toBe('WLAN');
  });
});

describe('held-back firmware updates (#142, #149)', () => {
  it('words the hub reasons and passes unknown ones on', () => {
    expect(skipReasonText('the hub battery is too low')).toBe('Akku zu schwach');
    expect(skipReasonText('the cellular signal is too weak')).toBe('Mobilfunksignal zu schwach');
    expect(skipReasonText('something new')).toBe('something new');
  });
});

describe('hub memory (#149)', () => {
  it('words the lowest free memory', () => {
    expect(memoryText({ heapMin: 6800 })).toBe('intern min. 6.6 KB frei');
    expect(
      memoryText({ heapMin: 41_200, psramMin: 3_250_000, stackSession: 1400, stackUplink: 2048 }),
    ).toBe('intern min. 40.2 KB frei · PSRAM min. 3.1 MB · Stack: Sitzung 1.4 KB, Upload 2.0 KB');
  });
});

describe('badgeText', () => {
  it('is short and names the contact time for a requested video', () => {
    expect(badgeText('new', false, undefined, 1_000_000)).toBe('Neu');
    expect(badgeText('videoReady', true, undefined, 1_000_000)).toBe('Gespeichert');
    expect(badgeText('videoReady', false, undefined, 1_000_000)).toBe('Video da');
    expect(badgeText('videoRequested', false, undefined, 1_000_000)).toBe(
      'Kommt beim nächsten Kontakt',
    );
    expect(badgeText('videoRequested', false, 1_000_600, 1_000_000)).toMatch(
      /^Kommt gegen \d\d:\d\d Uhr$/,
    );
    expect(badgeText('failed', false, undefined, 1_000_000)).toBe('Fehlgeschlagen');
  });
});
