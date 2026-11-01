import { describe, expect, it } from 'vitest';
import {
  batteryLevel,
  canRequestVideo,
  formatCameraTime,
  formatDateTime,
  formatRelative,
  formatStorage,
  mediaKey,
  toCameraWallClock,
  videoStateLabel,
} from './format';

describe('mediaKey', () => {
  it('matches the firmware thumbnail naming <dir>_<num>', () => {
    expect(mediaKey({ dir: 100, num: 36 })).toBe('100_36');
  });
});

describe('batteryLevel', () => {
  it.each([
    [100, 'ok'],
    [26, 'ok'],
    [25, 'low'],
    [11, 'low'],
    [10, 'critical'],
    [0, 'critical'],
  ] as const)('%i %% is %s', (percent, level) => {
    expect(batteryLevel(percent)).toBe(level);
  });
});

describe('formatStorage', () => {
  it('uses MB below 1 GB', () => {
    expect(formatStorage(512)).toBe('512 MB');
  });
  it('uses GB with one decimal from 1 GB', () => {
    expect(formatStorage(28_412)).toBe('27.7 GB');
  });
});

describe('formatRelative', () => {
  const now = 1_000_000;
  it.each([
    [now, 'just now'],
    [now - 59, 'just now'],
    [now - 17 * 60, '17 min ago'],
    [now - 3 * 3_600, '3 h ago'],
    [now - 86_400, '1 day ago'],
    [now - 3 * 86_400, '3 days ago'],
    [now + 120, 'just now'],
  ])('%i → %s', (ts, expected) => {
    expect(formatRelative(ts, now)).toBe(expected);
  });
});

describe('canRequestVideo', () => {
  it('allows videos that are not downloaded or failed', () => {
    expect(canRequestVideo({ fileType: 2, videoState: 'none' })).toBe(true);
    expect(canRequestVideo({ fileType: 2, videoState: 'failed' })).toBe(true);
  });
  it('rejects photos and videos already on their way', () => {
    expect(canRequestVideo({ fileType: 0, videoState: 'none' })).toBe(false);
    expect(canRequestVideo({ fileType: 2, videoState: 'requested' })).toBe(false);
    expect(canRequestVideo({ fileType: 2, videoState: 'available' })).toBe(false);
  });
});

describe('videoStateLabel', () => {
  it('explains that requests are served at the next wake-up', () => {
    expect(videoStateLabel('requested')).toContain('next wake-up');
  });
});

describe('formatCameraTime', () => {
  // 15.01.2026 and 15.07.2026, 14:00 on the camera (wall clock read as UTC).
  const winter = 1_768_485_600;
  const summer = 1_784_124_000;

  it('shows the camera wall clock as it is, whatever the browser zone', () => {
    expect(formatCameraTime(winter, 'en-GB')).toBe('15/01, 14:00');
    expect(formatCameraTime(summer, 'de-DE')).toBe('15.07., 14:00');
  });

  it('matches the local time of the real instant', () => {
    // A photo taken at a real instant shows the same time as that instant
    // formatted in the browser zone (which the camera clock was set to).
    for (const instant of [winter - 3_600, summer - 7_200, 1_793_484_891]) {
      expect(formatCameraTime(toCameraWallClock(instant), 'en-GB')).toBe(
        formatDateTime(instant, 'en-GB'),
      );
    }
  });
});
