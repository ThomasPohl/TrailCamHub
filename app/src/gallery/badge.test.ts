import { describe, expect, it, vi } from 'vitest';
import { showFaultBadge } from './badge';

describe('fault count on the app icon (#265)', () => {
  it('sets the count and clears it at zero', () => {
    const nav = {
      setAppBadge: vi.fn(() => Promise.resolve()),
      clearAppBadge: vi.fn(() => Promise.resolve()),
    };
    showFaultBadge(3, nav);
    expect(nav.setAppBadge).toHaveBeenCalledWith(3);
    showFaultBadge(0, nav);
    expect(nav.clearAppBadge).toHaveBeenCalledTimes(1);
  });

  it('does nothing without the Badging API and ignores refusals', () => {
    expect(() => showFaultBadge(2, {})).not.toThrow();
    expect(() => showFaultBadge(2, undefined)).not.toThrow();
    showFaultBadge(1, { setAppBadge: () => Promise.reject(new Error('denied')) });
  });
});
