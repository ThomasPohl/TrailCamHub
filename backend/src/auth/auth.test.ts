import { describe, expect, it } from 'vitest';
import { generateDeviceKey, parseDeviceKey, verifySecret } from './device';

describe('device keys', () => {
  it('generates keys that verify against their hash', () => {
    const { key, keyHash } = generateDeviceKey('CAM1');
    const parsed = parseDeviceKey(key);
    expect(parsed?.id).toBe('CAM1');
    expect(verifySecret(parsed?.secret ?? '', keyHash)).toBe(true);
  });

  it('rejects a wrong secret', () => {
    const { keyHash } = generateDeviceKey('CAM1');
    expect(verifySecret('x'.repeat(43), keyHash)).toBe(false);
  });

  it.each(['', 'CAM1', 'CAM1.', '.secretsecretsecret', 'CAM 1.secretsecretsecret', 'C.short'])(
    'rejects malformed key %j',
    (key) => {
      expect(parseDeviceKey(key)).toBeUndefined();
    },
  );
});
