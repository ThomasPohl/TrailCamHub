import { describe, expect, it } from 'vitest';
import { sparkPoints } from './sparkline';

describe('battery sparkline on the hub cards (#431)', () => {
  const now = 30 * 86_400;

  it('maps the last 30 days into the box, 0 % at the bottom', () => {
    expect(
      sparkPoints(
        [
          { at: 15 * 86_400, batPercent: 50 },
          { at: 0, batPercent: 100 },
          { at: now, batPercent: 0 },
        ],
        now,
      ),
    ).toBe('0.0,0.0 60.0,14.0 120.0,28.0');
  });

  it('leaves out old samples, charging and missing values', () => {
    expect(
      sparkPoints(
        [
          { at: -86_400, batPercent: 90 },
          { at: 10 * 86_400, batPercent: 80, charging: true },
          { at: 20 * 86_400 },
          { at: 25 * 86_400, batPercent: 70 },
        ],
        now,
      ),
    ).toBeUndefined();
  });
});
