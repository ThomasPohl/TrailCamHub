import { describe, expect, it } from 'vitest';
import { cameraFileName, cameraSuffix, cameraTime, videoFileName } from './text';

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

  it("names the camera's files as on its SD card", () => {
    expect(cameraFileName(12)).toBe('DSCF0012');
    expect(cameraSuffix(0)).toBe('JPG');
    expect(cameraSuffix(2)).toBe('AVI');
  });
});
