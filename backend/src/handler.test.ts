import type { APIGatewayProxyEventV2 } from 'aws-lambda';
import { describe, expect, it } from 'vitest';
import { toApiRequest } from './handler';

function event(overrides: Partial<APIGatewayProxyEventV2>): APIGatewayProxyEventV2 {
  return {
    version: '2.0',
    routeKey: '$default',
    rawPath: '/v1/cameras',
    rawQueryString: '',
    headers: {},
    isBase64Encoded: false,
    requestContext: { http: { method: 'get' } },
    ...overrides,
  } as APIGatewayProxyEventV2;
}

describe('toApiRequest', () => {
  it('normalizes method and header names', () => {
    const req = toApiRequest(event({ headers: { Authorization: 'Bearer abc' } }));
    expect(req).toEqual({
      method: 'GET',
      path: '/v1/cameras',
      headers: { authorization: 'Bearer abc' },
      body: undefined,
    });
  });

  it('decodes base64 bodies', () => {
    const req = toApiRequest(
      event({ body: Buffer.from('{"a":1}').toString('base64'), isBase64Encoded: true }),
    );
    expect(req.body).toBe('{"a":1}');
  });
});
