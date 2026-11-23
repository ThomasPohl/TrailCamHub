import { describe, expect, it } from 'vitest';
import { hubStatusLine } from './hubStatus';

const NOW = 1_000_000;

describe('hubStatusLine', () => {
  it('is silent before the first contact', () => {
    expect(hubStatusLine(0, undefined, NOW)).toEqual({
      light: 'silent',
      title: 'Der Hub hat sich noch nicht gemeldet',
    });
  });

  it('is fine until 5 minutes after the announced contact', () => {
    expect(hubStatusLine(NOW - 600, NOW + 60, NOW).light).toBe('ok');
    expect(hubStatusLine(NOW - 600, NOW - 299, NOW).light).toBe('ok');
  });

  it('is late, then silent, once the announced contact is missed', () => {
    expect(hubStatusLine(NOW - 4000, NOW - 301, NOW)).toMatchObject({
      light: 'late',
      title: 'Kontakt verspätet',
    });
    expect(hubStatusLine(NOW - 4000, NOW - 1801, NOW)).toMatchObject({
      light: 'silent',
      title: 'Der Hub meldet sich nicht',
    });
  });

  it('judges by the last contact when nothing was announced', () => {
    expect(hubStatusLine(NOW - 3600, undefined, NOW).light).toBe('ok');
    expect(hubStatusLine(NOW - 24 * 3600 - 2000, undefined, NOW).light).toBe('silent');
  });
});
