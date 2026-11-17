/**
 * The newest firmware release (#149): hubs install it from GitHub by
 * themselves (`releases/latest`), so the admin view compares each hub's
 * firmware with it.
 */

export const LATEST_RELEASE_URL =
  'https://api.github.com/repos/ThomasPohl/TrailCamHub/releases/latest';

/** "v1.2.3" (also with a suffix such as "-dirty") as numbers; undefined otherwise. */
export function parseVersion(text: string | undefined): [number, number, number] | undefined {
  const m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(text?.trim() ?? '');
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : undefined;
}

/** Whether `latest` is a newer release than `running`; false if either is unknown. */
export function isNewer(latest: string | undefined, running: string | undefined): boolean {
  const a = parseVersion(latest);
  const b = parseVersion(running);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) {
    const x = a[i] ?? 0;
    const y = b[i] ?? 0;
    if (x !== y) return x > y;
  }
  return false;
}

/** The tag of the latest release; undefined if GitHub does not answer. */
export async function fetchLatestRelease(
  fetchFn: typeof fetch = (...args) => fetch(...args),
): Promise<string | undefined> {
  try {
    const response = await fetchFn(LATEST_RELEASE_URL, {
      headers: { Accept: 'application/vnd.github+json' },
    });
    if (!response.ok) return undefined;
    const body = (await response.json()) as { tag_name?: unknown };
    return typeof body.tag_name === 'string' ? body.tag_name : undefined;
  } catch {
    return undefined;
  }
}
