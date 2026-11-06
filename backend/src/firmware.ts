/**
 * Firmware updates for hubs (see docs/ota.md). The CI publishes every release
 * (git tag vX.Y.Z) on GitHub with an `ota.json` manifest that lists one
 * application image per board and build variant. The backend reads the
 * manifest of the latest release and offers its image to a hub at check-in
 * when it is newer than the release the hub runs. Development builds (a
 * commit hash as version) get no offer.
 */

export interface FirmwareImage {
  /** https URL of the application image (GitHub release asset). */
  url: string;
  /** SHA-256 of the image, lowercase hex. */
  sha256: string;
  size: number;
}

export interface FirmwareManifest {
  version: string;
  /** Keyed by `<board>-<variant>`, e.g. `esp32s3-release`. */
  images: Record<string, FirmwareImage>;
}

/** What the check-in response offers; mirrors FirmwareOffer in the firmware. */
export interface FirmwareOffer extends FirmwareImage {
  version: string;
}

const VERSION = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const SHA256 = /^[0-9a-f]{64}$/;
const NAME = /^[a-z0-9-]{1,40}$/;

/** [major, minor, patch] of a plain release version, else undefined. */
export function parseVersion(text: unknown): [number, number, number] | undefined {
  if (typeof text !== 'string') return undefined;
  const match = VERSION.exec(text);
  if (!match) return undefined;
  const parts = match.slice(1, 4).map(Number) as [number, number, number];
  return parts.every(Number.isSafeInteger) ? parts : undefined;
}

export function isNewer(offered: string, running: string): boolean {
  const a = parseVersion(offered);
  const b = parseVersion(running);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) return (a[i] ?? 0) > (b[i] ?? 0);
  }
  return false;
}

/** The manifest if it is well-formed; images that are not are left out. */
export function parseManifest(raw: unknown): FirmwareManifest | undefined {
  if (typeof raw !== 'object' || raw === null) return undefined;
  const r = raw as Record<string, unknown>;
  if (!parseVersion(r.version) || typeof r.images !== 'object' || r.images === null) {
    return undefined;
  }
  const images: Record<string, FirmwareImage> = {};
  for (const [key, value] of Object.entries(r.images as Record<string, unknown>)) {
    const image = value as Partial<FirmwareImage> | null;
    if (
      NAME.test(key) &&
      typeof image?.url === 'string' &&
      image.url.startsWith('https://') &&
      typeof image.sha256 === 'string' &&
      SHA256.test(image.sha256) &&
      Number.isSafeInteger(image.size) &&
      (image.size as number) > 0
    ) {
      images[key] = { url: image.url, sha256: image.sha256, size: image.size as number };
    }
  }
  return { version: r.version as string, images };
}

/** The image for a hub, if the manifest has one for it that is newer. */
export function firmwareOffer(
  manifest: FirmwareManifest | undefined,
  hub: { firmware?: unknown; board?: unknown; variant?: unknown },
): FirmwareOffer | undefined {
  if (!manifest || typeof hub.firmware !== 'string') return undefined;
  if (typeof hub.board !== 'string' || typeof hub.variant !== 'string') return undefined;
  const image = manifest.images[`${hub.board}-${hub.variant}`];
  if (!image || !isNewer(manifest.version, hub.firmware)) return undefined;
  return { version: manifest.version, ...image };
}

const CACHE_S = 10 * 60;
const FAILURE_CACHE_S = 60;

/**
 * Reads the manifest from `url`, at most every ten minutes per Lambda
 * container (a minute after a failure). A missing or broken manifest means
 * no offer; it never fails a check-in.
 */
export function cachedManifest(
  url: string,
  now: () => number,
  fetchJson: (url: string) => Promise<unknown> = defaultFetchJson,
): () => Promise<FirmwareManifest | undefined> {
  let cached: { until: number; manifest: FirmwareManifest | undefined } | undefined;
  return async () => {
    const t = now();
    if (cached && t < cached.until) return cached.manifest;
    try {
      const manifest = parseManifest(await fetchJson(url));
      if (!manifest) console.warn(`Firmware manifest ${url} is not valid`);
      cached = { until: t + (manifest ? CACHE_S : FAILURE_CACHE_S), manifest };
    } catch (error) {
      console.warn(`Firmware manifest ${url} could not be read`, error);
      cached = { until: t + FAILURE_CACHE_S, manifest: undefined };
    }
    return cached.manifest;
  };
}

async function defaultFetchJson(url: string): Promise<unknown> {
  // GitHub answers the "latest release" URL with a redirect to the asset.
  const response = await fetch(url, { redirect: 'follow', signal: AbortSignal.timeout(5_000) });
  if (response.status === 404) return undefined;
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}
