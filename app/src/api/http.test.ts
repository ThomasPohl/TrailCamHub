import { describe, expect, it, vi } from 'vitest';
import { createApiClient } from '.';
import { DemoApiClient } from './demo';
import { ApiError, HttpApiClient } from './http';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

describe('HttpApiClient', () => {
  it('sends the Telegram init data as tma authorization', async () => {
    const fetchFn = vi.fn<typeof fetch>().mockResolvedValue(jsonResponse([]));
    const client = new HttpApiClient('https://api.example.com/', 'query_id=1&hash=abc', fetchFn);

    await client.listCameras();

    expect(fetchFn).toHaveBeenCalledWith(
      'https://api.example.com/v1/cameras',
      expect.objectContaining({
        method: 'GET',
        headers: expect.objectContaining({ Authorization: 'tma query_id=1&hash=abc' }),
      }),
    );
  });

  it('omits the authorization header without init data', async () => {
    const fetchFn = vi.fn<typeof fetch>().mockResolvedValue(jsonResponse([]));
    await new HttpApiClient('https://api.example.com', '', fetchFn).listCameras();

    const init = fetchFn.mock.calls[0]?.[1];
    expect(init?.headers).not.toHaveProperty('Authorization');
  });

  it('posts a requestVideo command for the camera', async () => {
    const fetchFn = vi
      .fn<typeof fetch>()
      .mockResolvedValue(jsonResponse({ id: 'c1', type: 'requestVideo', state: 'queued' }));
    const client = new HttpApiClient('https://api.example.com', 'x', fetchFn);

    const result = await client.requestVideo('F4C15606ED3B', { dir: 100, num: 35 });

    expect(result.state).toBe('queued');
    const [url, init] = fetchFn.mock.calls[0] ?? [];
    expect(url).toBe('https://api.example.com/v1/cameras/F4C15606ED3B/commands');
    expect(init?.method).toBe('POST');
    expect(JSON.parse(String(init?.body))).toEqual({ type: 'requestVideo', dir: 100, num: 35 });
  });

  it('throws ApiError with the HTTP status on failure', async () => {
    const fetchFn = vi.fn<typeof fetch>().mockResolvedValue(jsonResponse({}, 401));
    const client = new HttpApiClient('https://api.example.com', 'x', fetchFn);

    await expect(client.listMedia('cam')).rejects.toMatchObject({
      name: 'ApiError',
      status: 401,
    } satisfies Partial<ApiError>);
  });

  it('includes the backend error message', async () => {
    const fetchFn = vi
      .fn<typeof fetch>()
      .mockResolvedValue(jsonResponse({ error: 'Invalid Telegram init data (bad signature)' }, 401));
    const client = new HttpApiClient('https://api.example.com', 'x', fetchFn);

    await expect(client.listCameras()).rejects.toThrow(
      'GET /v1/cameras failed with HTTP 401: Invalid Telegram init data (bad signature)',
    );
  });

  it('shows a non-JSON error body as text and keeps it short', async () => {
    const fetchFn = vi
      .fn<typeof fetch>()
      .mockResolvedValue(new Response('x'.repeat(500), { status: 502 }));
    const client = new HttpApiClient('https://api.example.com', 'x', fetchFn);

    const error = await client.listCameras().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(ApiError);
    expect((error as ApiError).message).toBe(
      `GET /v1/cameras failed with HTTP 502: ${'x'.repeat(200)}…`,
    );
  });

  it('keeps the plain message without an error body', async () => {
    const fetchFn = vi.fn<typeof fetch>().mockResolvedValue(new Response('', { status: 500 }));
    const client = new HttpApiClient('https://api.example.com', 'x', fetchFn);

    await expect(client.listCameras()).rejects.toThrow(/^GET \/v1\/cameras failed with HTTP 500$/);
  });
});

describe('createApiClient', () => {
  it('falls back to demo data without an API URL', () => {
    expect(createApiClient('', '')).toBeInstanceOf(DemoApiClient);
    expect(createApiClient('   ', '')).toBeInstanceOf(DemoApiClient);
  });

  it('uses the HTTP client when an API URL is configured', () => {
    expect(createApiClient('https://api.example.com', 'x')).toBeInstanceOf(HttpApiClient);
  });
});
