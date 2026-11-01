import type { ApiClient, Camera, CommandResult, MediaItem } from './types';

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

/**
 * Talks to the TrailCamHub backend.
 *
 * Authentication: the signed Telegram init data is sent as
 * `Authorization: tma <initData>`; the backend verifies its HMAC signature
 * with the bot token. The app itself never holds a secret.
 */
export class HttpApiClient implements ApiClient {
  readonly isDemo = false;
  private readonly baseUrl: string;

  constructor(
    baseUrl: string,
    private readonly initData: string,
    private readonly fetchFn: typeof fetch = (...args) => fetch(...args),
  ) {
    this.baseUrl = baseUrl.replace(/\/+$/, '');
  }

  listCameras(): Promise<Camera[]> {
    return this.request<Camera[]>('GET', '/v1/cameras');
  }

  listMedia(camId: string): Promise<MediaItem[]> {
    return this.request<MediaItem[]>('GET', `/v1/cameras/${encodeURIComponent(camId)}/media`);
  }

  requestVideo(camId: string, item: Pick<MediaItem, 'dir' | 'num'>): Promise<CommandResult> {
    return this.request<CommandResult>(
      'POST',
      `/v1/cameras/${encodeURIComponent(camId)}/commands`,
      {
        type: 'requestVideo',
        dir: item.dir,
        num: item.num,
      },
    );
  }

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    const headers: Record<string, string> = { Accept: 'application/json' };
    if (this.initData) {
      headers.Authorization = `tma ${this.initData}`;
    }
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
    }
    const response = await this.fetchFn(this.baseUrl + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) {
      const detail = await errorDetail(response);
      throw new ApiError(
        response.status,
        `${method} ${path} failed with HTTP ${response.status}${detail ? `: ${detail}` : ''}`,
      );
    }
    return (await response.json()) as T;
  }
}

/** Longest backend error text shown to the user. */
const MAX_DETAIL_LENGTH = 200;

/**
 * Reads the backend's `{ "error": "…" }` body so the user sees why a request
 * failed (e.g. a bad init data signature), not just the status code.
 */
async function errorDetail(response: Response): Promise<string | undefined> {
  let text: string;
  try {
    text = await response.text();
  } catch {
    return undefined;
  }
  let detail = text;
  try {
    const parsed: unknown = JSON.parse(text);
    if (typeof parsed === 'object' && parsed !== null && 'error' in parsed) {
      const { error } = parsed as { error: unknown };
      if (typeof error === 'string') detail = error;
    }
  } catch {
    // Not JSON: show the plain text.
  }
  detail = detail.trim();
  if (!detail) return undefined;
  return detail.length > MAX_DETAIL_LENGTH ? `${detail.slice(0, MAX_DETAIL_LENGTH)}…` : detail;
}
