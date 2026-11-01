import { GetParameterCommand, SSMClient } from '@aws-sdk/client-ssm';
import type { APIGatewayProxyEventV2, APIGatewayProxyStructuredResultV2 } from 'aws-lambda';
import { handleAdmin, isAdminEvent, type AdminEvent, type AdminResult } from './admin';
import { handleRequest, type Deps } from './api';
import type { ApiRequest } from './http';
import { S3MediaStorage } from './storage';
import { DynamoStore } from './store/dynamo';

function requireEnv(name: string): string {
  const value = process.env[name];
  if (!value) throw new Error(`Missing environment variable ${name}`);
  return value;
}

let botTokenPromise: Promise<string> | undefined;

/**
 * The bot token comes from the environment (set at deploy time) or, if that
 * is empty, from SSM Parameter Store - read once per Lambda container.
 */
export function botToken(env: NodeJS.ProcessEnv = process.env): Promise<string> {
  const direct = env.TELEGRAM_BOT_TOKEN?.trim();
  if (direct) return Promise.resolve(direct);
  const parameter = env.TELEGRAM_BOT_TOKEN_PARAM?.trim();
  if (!parameter) {
    return Promise.reject(
      new Error('No Telegram bot token: set TELEGRAM_BOT_TOKEN or TELEGRAM_BOT_TOKEN_PARAM'),
    );
  }
  botTokenPromise ??= new SSMClient({})
    .send(new GetParameterCommand({ Name: parameter, WithDecryption: true }))
    .then((result) => {
      const value = result.Parameter?.Value;
      if (!value) throw new Error('Telegram bot token parameter is empty');
      return value;
    })
    .catch((error: unknown) => {
      botTokenPromise = undefined; // retry on the next request
      throw error;
    });
  return botTokenPromise;
}

let deps: Deps | undefined;

function getDeps(): Deps {
  deps ??= {
    store: new DynamoStore(requireEnv('TABLE_NAME')),
    storage: new S3MediaStorage(requireEnv('MEDIA_BUCKET')),
    botToken: () => botToken(),
    now: () => Math.floor(Date.now() / 1000),
    config: {
      sleepIntervalS: Number(process.env.SLEEP_INTERVAL_S) || undefined,
    },
  };
  return deps;
}

export function toApiRequest(event: APIGatewayProxyEventV2): ApiRequest {
  const headers: Record<string, string | undefined> = {};
  for (const [key, value] of Object.entries(event.headers ?? {})) {
    headers[key.toLowerCase()] = value;
  }
  const body =
    event.body && event.isBase64Encoded
      ? Buffer.from(event.body, 'base64').toString('utf8')
      : event.body;
  return {
    method: event.requestContext.http.method.toUpperCase(),
    path: event.rawPath,
    headers,
    body,
  };
}

/**
 * Lambda entry point. Function URL requests (payload format 2.0, CORS handled
 * by the URL config) go to the API; direct invocations with an `admin` field
 * run an admin task (see admin.ts).
 */
export async function handler(
  event: APIGatewayProxyEventV2 | AdminEvent,
): Promise<APIGatewayProxyStructuredResultV2 | AdminResult> {
  if (isAdminEvent(event)) return handleAdmin(event, getDeps().store);
  return handleRequest(toApiRequest(event as APIGatewayProxyEventV2), getDeps());
}
