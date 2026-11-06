import { generateDeviceKey } from './auth/device';
import { signInitData } from './auth/telegram';
import type { Deps } from './api';
import type { ApiRequest } from './http';
import type { MediaStorage } from './storage';
import { MemoryStore } from './store/memory';
import {
  TelegramError,
  webhookSecret,
  type PhotoUpload,
  type SendOptions,
  type TelegramApi,
} from './telegram/api';
import type { TelegramUpdate } from './telegram/bot';

export const BOT_TOKEN = '123456:TEST-token-for-unit-tests';
export const NOW = 1_800_000_000;
export const USER_ID = 4242;
export const HUB_ID = 'garden-hub';
export const CAM_ID = 'F4C15606ED3B';
export const CAM_2 = 'A1B2C3D4E5F6';

/** Builds init data the way Telegram does, signed with BOT_TOKEN. */
export function makeInitData(userId = USER_ID, authDate = NOW - 60, botToken = BOT_TOKEN): string {
  const params = new URLSearchParams({
    query_id: 'AAH_test',
    user: JSON.stringify({ id: userId, first_name: 'Thomas' }),
    auth_date: String(authDate),
  });
  params.set('hash', signInitData(params, botToken));
  return params.toString();
}

export class FakeStorage implements MediaStorage {
  readonly uploaded = new Set<string>();
  async thumbReadUrl(camId: string, mediaId: number) {
    return `https://media.test/get/${camId}/${mediaId}.jpg`;
  }
  async thumbUploadUrl(camId: string, mediaId: number) {
    return `https://media.test/put/${camId}/${mediaId}.jpg`;
  }
  async thumbExists(camId: string, mediaId: number) {
    return this.uploaded.has(`${camId}/${mediaId}`);
  }
  readonly uploadedVideos = new Set<string>();
  async videoReadUrl(camId: string, mediaId: number) {
    return `https://media.test/get/${camId}/${mediaId}.mp4`;
  }
  async videoUploadUrl(camId: string, mediaId: number) {
    return `https://media.test/put/${camId}/${mediaId}.mp4`;
  }
  async videoExists(camId: string, mediaId: number) {
    return this.uploadedVideos.has(`${camId}/${mediaId}`);
  }
}

export const APP_URL = 'https://app.test/app/';

/** Records what the bot sends instead of calling Telegram. */
export class FakeTelegram implements TelegramApi {
  readonly messages: Array<{ chatId: number; text: string; options?: SendOptions }> = [];
  readonly photos: Array<{
    chatId: number;
    photo: string | PhotoUpload;
    caption: string;
    options?: SendOptions;
  }> = [];
  readonly answers: Array<{ id: string; text?: string }> = [];
  /** Chats that answer 403 (bot blocked or never started). */
  readonly blocked = new Set<number>();

  async botUsername() {
    return 'TrailCamHubBot';
  }

  async sendMessage(chatId: number, text: string, options?: SendOptions) {
    if (this.blocked.has(chatId)) throw new TelegramError(403, 'Forbidden: bot was blocked');
    this.messages.push({ chatId, text, options });
  }

  async sendPhoto(
    chatId: number,
    photo: string | PhotoUpload,
    caption: string,
    options?: SendOptions,
  ) {
    if (this.blocked.has(chatId)) throw new TelegramError(403, 'Forbidden: bot was blocked');
    this.photos.push({ chatId, photo, caption, options });
  }

  async answerCallbackQuery(id: string, text?: string) {
    this.answers.push({ id, text });
  }

  /** Text of the last message to the chat. */
  lastText(chatId = USER_ID): string {
    return [...this.messages].reverse().find((m) => m.chatId === chatId)?.text ?? '';
  }
}

let updateId = 1;

/** A webhook request as Telegram sends it. */
export function webhookRequest(update: Omit<TelegramUpdate, 'update_id'>, secret?: string) {
  return {
    method: 'POST',
    path: '/v1/telegram/webhook',
    headers: { 'x-telegram-bot-api-secret-token': secret ?? webhookSecret(BOT_TOKEN) },
    body: JSON.stringify({ update_id: updateId++, ...update }),
  } satisfies ApiRequest;
}

/** A private-chat text message from the user. */
export function textMessage(text: string, userId = USER_ID): Omit<TelegramUpdate, 'update_id'> {
  return {
    message: {
      message_id: 1,
      from: { id: userId },
      chat: { id: userId, type: 'private' },
      text,
    },
  };
}

/** A button press in the user's private chat. */
export function buttonPress(data: string, userId = USER_ID): Omit<TelegramUpdate, 'update_id'> {
  return {
    callback_query: {
      id: `cb-${data}`,
      from: { id: userId },
      data,
      message: { chat: { id: userId, type: 'private' } },
    },
  };
}

/** A store with one hub linked to USER_ID, no cameras yet. */
export async function setup() {
  const store = new MemoryStore();
  const storage = new FakeStorage();
  const hub = generateDeviceKey(HUB_ID);
  await store.putHub({ hubId: HUB_ID, name: 'Garden hub', keyHash: hub.keyHash, lastSeen: 0 });
  await store.linkUser(USER_ID, HUB_ID);
  const other = generateDeviceKey('other-hub');
  await store.putHub({ hubId: 'other-hub', name: 'Other', keyHash: other.keyHash, lastSeen: 0 });
  let nextId = 1;
  const telegram = new FakeTelegram();
  const deps: Deps = {
    store,
    storage,
    botToken: async () => BOT_TOKEN,
    now: () => NOW,
    newId: () => `cmd-${nextId++}`,
    telegram: { api: telegram, appUrl: APP_URL, defaultTimeZone: 'Europe/Berlin' },
  };
  return { store, storage, deps, telegram, hubKey: hub.key, otherHubKey: other.key };
}

export function appRequest(
  method: string,
  path: string,
  body?: unknown,
  initData = makeInitData(),
): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `tma ${initData}` },
    body: body === undefined ? undefined : JSON.stringify(body),
  };
}

export function hubRequest(path: string, hubKey: string, body: unknown): ApiRequest {
  return {
    method: 'POST',
    path,
    headers: { authorization: `Bearer ${hubKey}` },
    body: JSON.stringify(body),
  };
}
