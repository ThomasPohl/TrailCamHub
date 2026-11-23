import { describe, expect, it } from 'vitest';
import type { FirmwareChange } from './model';
import { selectFirmwareChanges } from './hub-inputs';

const change = (at: number, result?: FirmwareChange['result']): FirmwareChange => ({
  at,
  from: '0.1.0',
  to: '0.1.1',
  ...(result ? { result } : {}),
});

describe('selectFirmwareChanges (#363)', () => {
  it('keeps a failure that many newer skipped events would push out of a shared window', () => {
    const failed = change(100, 'failed');
    const skipped = Array.from({ length: 12 }, (_, i) => change(200 + i, 'skipped'));
    const kept = selectFirmwareChanges([failed, ...skipped]);
    expect(kept).toContainEqual(failed);
    expect(kept.filter((c) => c.result === 'skipped')).toHaveLength(5);
  });

  it('keeps five of each kind, newest first', () => {
    const others = Array.from({ length: 8 }, (_, i) => change(10 + i));
    const kept = selectFirmwareChanges(others);
    expect(kept.map((c) => c.at)).toEqual([17, 16, 15, 14, 13]);
  });
});
