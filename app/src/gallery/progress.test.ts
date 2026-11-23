import { describe, expect, it } from 'vitest';
import { progressSteps } from './progress';

const NOW = 1_000_000;

describe('progressSteps', () => {
  it('is only the thumbnail for a photo', () => {
    expect(progressSteps({ state: 'new', fileType: 0 }, false, undefined, NOW)).toEqual([
      { text: 'Vorschaubild ist da', status: 'done' },
    ]);
  });

  it('shows a new video as not asked for yet', () => {
    const steps = progressSteps({ state: 'new', fileType: 2 }, false, undefined, NOW);
    expect(steps.map((s) => s.status)).toEqual(['done', 'todo', 'todo']);
  });

  it('names the contact time while the hub fetches the video', () => {
    const steps = progressSteps({ state: 'videoRequested', fileType: 2 }, false, NOW + 600, NOW);
    expect(steps[1]?.status).toBe('current');
    expect(steps[1]?.text).toMatch(/^Video angefordert – der Hub holt es gegen \d\d:\d\d Uhr$/);
  });

  it('tells a finished video from a saved one', () => {
    const ready = progressSteps({ state: 'videoReady', fileType: 2 }, false, undefined, NOW);
    expect(ready.map((s) => s.status)).toEqual(['done', 'done', 'current']);
    const saved = progressSteps({ state: 'videoReady', fileType: 2 }, true, undefined, NOW);
    expect(saved[2]).toEqual({ text: 'Im Ordner gespeichert', status: 'done' });
  });

  it('reports a failure with its reason', () => {
    const steps = progressSteps(
      { state: 'failed', fileType: 2, failReason: 'Die Kamera war nicht erreichbar.' },
      false,
      undefined,
      NOW,
    );
    expect(steps[1]).toEqual({
      text: 'Hat nicht geklappt: Die Kamera war nicht erreichbar.',
      status: 'failed',
    });
  });
});
