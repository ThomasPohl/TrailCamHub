import { describe, expect, it } from 'vitest';
import { cameraFile, cameraTime, folderName, videoFileName } from './text';

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
});
