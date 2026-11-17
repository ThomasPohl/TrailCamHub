import { describe, expect, it } from 'vitest';
import { fetchLatestRelease, isNewer, parseVersion } from './release';

describe('latest firmware release (#149)', () => {
  it('compares versions', () => {
    expect(parseVersion('v0.1.13')).toEqual([0, 1, 13]);
    expect(parseVersion('0.2.0-dirty')).toEqual([0, 2, 0]);
    expect(parseVersion('dev')).toBeUndefined();
    expect(isNewer('v0.1.14', 'v0.1.13')).toBe(true);
    expect(isNewer('v0.2.0', 'v0.1.99')).toBe(true);
    expect(isNewer('v0.1.13', 'v0.1.13')).toBe(false);
    expect(isNewer('v0.1.12', 'v0.1.13')).toBe(false);
    expect(isNewer('v0.1.14', undefined)).toBe(false);
    expect(isNewer(undefined, 'v0.1.13')).toBe(false);
  });

  it('reads the tag and gives up quietly', async () => {
    const ok = (async () => new Response(JSON.stringify({ tag_name: 'v0.1.14' }))) as typeof fetch;
    expect(await fetchLatestRelease(ok)).toBe('v0.1.14');
    const down = (async () => new Response('', { status: 403 })) as typeof fetch;
    expect(await fetchLatestRelease(down)).toBeUndefined();
    const offline = (async () => {
      throw new Error('offline');
    }) as typeof fetch;
    expect(await fetchLatestRelease(offline)).toBeUndefined();
  });
});
