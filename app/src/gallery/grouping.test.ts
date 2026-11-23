import { describe, expect, it } from 'vitest';
import { toCameraWallClock } from '../format';
import { applyFilter, countByFilter, groupByDay, newestFirst, recordingKey } from './grouping';
import type { Recording, RecordingState } from './types';

const NOW = new Date(2026, 10, 23, 12, 0, 0).getTime() / 1000; // local noon, so the test holds in any zone
const wall = (h: number, daysAgo = 0) =>
  toCameraWallClock(NOW) - daysAgo * 86_400 + (h - 12) * 3600;

function rec(
  mediaId: number,
  mediaTime: number,
  state: RecordingState = 'new',
  camId = 'cam',
): Recording {
  return {
    mediaId,
    mediaIds: [mediaId],
    camId,
    fileType: 2,
    cameraFiles: [],
    mediaTime,
    thumbUrl: '',
    state,
    fileName: '',
  };
}

describe('grouping', () => {
  it('sorts newest first across cameras', () => {
    const sorted = newestFirst([rec(1, 10, 'new', 'a'), rec(2, 30, 'new', 'b'), rec(3, 20)]);
    expect(sorted.map((r) => r.mediaId)).toEqual([2, 3, 1]);
  });

  it('groups by day with Heute and Gestern', () => {
    const items = newestFirst([
      rec(1, wall(10)),
      rec(2, wall(8)),
      rec(3, wall(22, 1)),
      rec(4, wall(9, 3)),
    ]);
    const groups = groupByDay(items, NOW);
    expect(groups.map((g) => g.title)).toEqual(['Heute', 'Gestern', 'Donnerstag, 19. November']);
    expect(groups[0]?.subtitle).toBe('Montag, 23. November');
    expect(groups[0]?.items.map((r) => r.mediaId)).toEqual([1, 2]);
    expect(groups[2]?.subtitle).toBe('');
  });

  it('counts and filters by state; waiting covers both requests', () => {
    const items = [
      rec(1, 1, 'new'),
      rec(2, 2, 'videoRequested'),
      rec(3, 3, 'deleteRequested'),
      rec(4, 4, 'videoReady'),
      rec(5, 5, 'failed'),
    ];
    expect(countByFilter(items)).toEqual({ all: 5, new: 1, waiting: 2, ready: 1, failed: 1 });
    expect(applyFilter(items, 'waiting').map((r) => r.mediaId)).toEqual([2, 3]);
    expect(applyFilter(items, 'all')).toHaveLength(5);
  });

  it('keys a recording by camera and number', () => {
    expect(recordingKey(rec(7, 1, 'new', 'abc'))).toBe('abc/7');
  });
});
