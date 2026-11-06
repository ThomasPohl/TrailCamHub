import { describe, expect, it, vi } from 'vitest';
import { handleRequest } from './api';
import {
  cachedManifest,
  firmwareOffer,
  isNewer,
  parseManifest,
  parseVersion,
  type FirmwareManifest,
} from './firmware';
import type { ApiResponse } from './http';
import type { CheckinResponse } from './model';
import { hubRequest, setup } from './test-utils';

const SHA = 'a'.repeat(64);
const URL_S3 =
  'https://github.com/ThomasPohl/TrailCamHub/releases/download/v1.3.0/trailcamhub-esp32s3-release.app.bin';

const rawManifest = {
  version: 'v1.3.0',
  images: {
    'esp32s3-release': { url: URL_S3, sha256: SHA, size: 1_572_864 },
    'esp32s3-dev': { url: URL_S3.replace('release', 'dev'), sha256: SHA, size: 1_900_000 },
    'esp32p4-release': { url: 'http://insecure.example/x.bin', sha256: SHA, size: 1 },
    'tdongle-c5-release': { url: URL_S3, sha256: 'ABC', size: 1 },
  },
};

describe('firmware versions', () => {
  it('parses only plain release versions', () => {
    expect(parseVersion('v1.2.3')).toEqual([1, 2, 3]);
    expect(parseVersion('10.0.12')).toEqual([10, 0, 12]);
    for (const text of ['358b4b9', 'v1.2', 'v1.2.3-rc1', 'v1.2.3-4-gabc', 'v01.2.3', '', 7]) {
      expect(parseVersion(text)).toBeUndefined();
    }
  });

  it('compares by major, minor, patch', () => {
    expect(isNewer('v1.10.0', 'v1.9.9')).toBe(true);
    expect(isNewer('v2.0.0', 'v1.99.99')).toBe(true);
    expect(isNewer('v1.2.3', 'v1.2.3')).toBe(false);
    expect(isNewer('v1.2.2', 'v1.2.3')).toBe(false);
    // A development build is never offered an update.
    expect(isNewer('v1.3.0', '358b4b9')).toBe(false);
  });
});

describe('firmware manifest', () => {
  it('keeps only well-formed https images', () => {
    const manifest = parseManifest(rawManifest);
    expect(manifest?.version).toBe('v1.3.0');
    expect(Object.keys(manifest?.images ?? {}).sort()).toEqual(['esp32s3-dev', 'esp32s3-release']);
    expect(parseManifest({ version: 'latest', images: {} })).toBeUndefined();
    expect(parseManifest(null)).toBeUndefined();
  });

  it('offers the image of the hub board and variant when it is newer', () => {
    const manifest = parseManifest(rawManifest) as FirmwareManifest;
    const hub = { firmware: 'v1.2.0', board: 'esp32s3', variant: 'release' };
    expect(firmwareOffer(manifest, hub)).toEqual({
      version: 'v1.3.0',
      url: URL_S3,
      sha256: SHA,
      size: 1_572_864,
    });
    expect(firmwareOffer(manifest, { ...hub, firmware: 'v1.3.0' })).toBeUndefined();
    expect(firmwareOffer(manifest, { ...hub, firmware: '358b4b9' })).toBeUndefined();
    expect(firmwareOffer(manifest, { ...hub, board: 'esp32p4' })).toBeUndefined();
    expect(firmwareOffer(manifest, { firmware: 'v1.2.0' })).toBeUndefined();
    expect(firmwareOffer(undefined, hub)).toBeUndefined();
  });

  it('is read at most every ten minutes, again a minute after a failure', async () => {
    let t = 1_000;
    const fetchJson = vi
      .fn<(url: string) => Promise<unknown>>()
      .mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValue(rawManifest);
    const read = cachedManifest('https://example.test/ota.json', () => t, fetchJson);
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});

    expect(await read()).toBeUndefined();
    t += 30;
    expect(await read()).toBeUndefined();
    expect(fetchJson).toHaveBeenCalledTimes(1);
    t += 31;
    expect((await read())?.version).toBe('v1.3.0');
    t += 9 * 60;
    await read();
    expect(fetchJson).toHaveBeenCalledTimes(2);
    t += 61;
    await read();
    expect(fetchJson).toHaveBeenCalledTimes(3);
    warn.mockRestore();
  });
});

describe('firmware offer at check-in', () => {
  const checkin = async (hub: Record<string, unknown>, manifest?: unknown) => {
    const ctx = await setup();
    ctx.deps.firmwareManifest = async () => parseManifest(manifest);
    const res: ApiResponse = await handleRequest(
      hubRequest('/v1/device/checkin', ctx.hubKey, { ...hub, cameras: [] }),
      ctx.deps,
    );
    expect(res.statusCode).toBe(200);
    return JSON.parse(res.body) as CheckinResponse;
  };

  it('offers a newer release to a hub running an older one', async () => {
    const response = await checkin(
      { hubFirmware: 'v1.2.0', hubBoard: 'esp32s3', hubVariant: 'release' },
      rawManifest,
    );
    expect(response.firmware).toEqual({
      version: 'v1.3.0',
      url: URL_S3,
      sha256: SHA,
      size: 1_572_864,
    });
  });

  it('offers nothing to a development build or without a manifest', async () => {
    expect(
      (
        await checkin(
          { hubFirmware: '358b4b9', hubBoard: 'esp32s3', hubVariant: 'dev' },
          rawManifest,
        )
      ).firmware,
    ).toBeUndefined();
    expect(
      (await checkin({ hubFirmware: 'v1.2.0', hubBoard: 'esp32s3', hubVariant: 'release' }))
        .firmware,
    ).toBeUndefined();
  });
});
