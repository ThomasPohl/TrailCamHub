import type { MediaItem, VideoState } from './api/types';

/** File-style key used across firmware, backend and app: `<dir>_<num>`. */
export function mediaKey(item: Pick<MediaItem, 'dir' | 'num'>): string {
  return `${item.dir}_${item.num}`;
}

export type BatteryLevel = 'ok' | 'low' | 'critical';

export function batteryLevel(percent: number): BatteryLevel {
  if (percent <= 10) return 'critical';
  if (percent <= 25) return 'low';
  return 'ok';
}

export function formatStorage(megabytes: number): string {
  if (megabytes >= 1024) {
    return `${(megabytes / 1024).toFixed(1)} GB`;
  }
  return `${Math.round(megabytes)} MB`;
}

/** "just now", "17 min ago", "3 h ago", "2 days ago". */
export function formatRelative(unixSeconds: number, nowSeconds: number): string {
  const diff = Math.max(0, nowSeconds - unixSeconds);
  if (diff < 60) return 'just now';
  if (diff < 3_600) return `${Math.floor(diff / 60)} min ago`;
  if (diff < 86_400) return `${Math.floor(diff / 3_600)} h ago`;
  const days = Math.floor(diff / 86_400);
  return days === 1 ? '1 day ago' : `${days} days ago`;
}

const DATE_TIME: Intl.DateTimeFormatOptions = {
  day: '2-digit',
  month: '2-digit',
  hour: '2-digit',
  minute: '2-digit',
};

/**
 * A real instant (Unix seconds), e.g. a check-in or sync time, in the time
 * zone of the browser.
 */
export function formatDateTime(unixSeconds: number, locale?: string): string {
  return new Date(unixSeconds * 1000).toLocaleString(locale, DATE_TIME);
}

/**
 * A time from the camera (`mediaTime`): its wall clock read as if it were UTC
 * (docs/clock.md). The camera has no time zone, so the value is shown as it
 * is, formatted in UTC; the browser's zone would shift it by its offset.
 */
export function formatCameraTime(cameraSeconds: number, locale?: string): string {
  return new Date(cameraSeconds * 1000).toLocaleString(locale, {
    ...DATE_TIME,
    timeZone: 'UTC',
  });
}

/**
 * The camera wall clock (as the camera would show it) for a real instant, by
 * the browser's time zone, the inverse of what the hub does in
 * camera_wall_clock_to_utc().
 */
export function toCameraWallClock(unixSeconds: number): number {
  return unixSeconds - new Date(unixSeconds * 1000).getTimezoneOffset() * 60;
}

export function videoStateLabel(state: VideoState): string {
  switch (state) {
    case 'none':
      return 'Not downloaded';
    case 'requested':
      return 'Requested · loads at next wake-up';
    case 'downloading':
      return 'Downloading…';
    case 'available':
      return 'Available';
    case 'failed':
      return 'Download failed';
  }
}

/** A video can be (re-)requested unless it is already on its way or there. */
export function canRequestVideo(item: Pick<MediaItem, 'fileType' | 'videoState'>): boolean {
  return item.fileType === 2 && (item.videoState === 'none' || item.videoState === 'failed');
}
