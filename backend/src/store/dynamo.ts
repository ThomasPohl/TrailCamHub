import { ConditionalCheckFailedException, DynamoDBClient } from '@aws-sdk/client-dynamodb';
import {
  DeleteCommand,
  DynamoDBDocumentClient,
  GetCommand,
  PutCommand,
  QueryCommand,
  ScanCommand,
  TransactWriteCommand,
  UpdateCommand,
  type QueryCommandInput,
  type ScanCommandInput,
} from '@aws-sdk/lib-dynamodb';
import type {
  AppUser,
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
  PairingCode,
  ScheduleConfig,
  UserSettings,
  VideoState,
} from '../model';
import type { CameraTelemetry, HubTelemetry, Store } from './types';

/*
 * Single-table layout (PK / SK):
 *   HUB#<hubId>  / META                   hub record
 *   HUB#<hubId>  / CAM#<camId>            hub → camera (listing)
 *   USER#<id>    / HUB#<hubId>            user → hub link
 *   HUB#<hubId>  / USER#<id>              hub → user link (who gets notified)
 *   USER#<id>    / SETTINGS               bot settings of the user
 *   USER#<id>    / APP                    gallery app user (token hash, role)
 *   PAIR#<code>  / META                   one-time pairing code (expires: ttl)
 *   CAM#<camId>  / META                   camera record (with hubId)
 *   CAM#<camId>  / MEDIA#<dir>#<num>      media (zero-padded, sorts by file)
 *   CAM#<camId>  / CMD#<id>               command
 */
const hubPk = (hubId: string) => `HUB#${hubId}`;
const camPk = (camId: string) => `CAM#${camId}`;
const userPk = (userId: number) => `USER#${userId}`;
const pairPk = (code: string) => `PAIR#${code}`;
const mediaSk = (mediaId: number) => `MEDIA#${mediaId}`;
const cmdSk = (id: string) => `CMD#${id}`;

function strip<T>(item: Record<string, unknown>): T {
  const { PK: _pk, SK: _sk, ...rest } = item;
  void _pk;
  void _sk;
  return rest as T;
}

function isConditionFailure(error: unknown): boolean {
  if (error instanceof ConditionalCheckFailedException) return true;
  // Transactions report failed conditions as cancellation reasons.
  const reasons = (error as { CancellationReasons?: Array<{ Code?: string }> }).CancellationReasons;
  return Array.isArray(reasons) && reasons.some((r) => r.Code === 'ConditionalCheckFailed');
}

export class DynamoStore implements Store {
  private readonly db: DynamoDBDocumentClient;

  constructor(
    private readonly table: string,
    client: DynamoDBClient = new DynamoDBClient({}),
  ) {
    this.db = DynamoDBDocumentClient.from(client, {
      marshallOptions: { removeUndefinedValues: true },
    });
  }

  // ---- Hubs ----------------------------------------------------------------

  async putHub(hub: HubRecord): Promise<void> {
    await this.db.send(
      new PutCommand({ TableName: this.table, Item: { PK: hubPk(hub.hubId), SK: 'META', ...hub } }),
    );
  }

  async getHub(hubId: string): Promise<HubRecord | undefined> {
    return this.getItem<HubRecord>(hubPk(hubId), 'META');
  }

  /**
   * A scan: hubs have no index item of their own. Only the admin asks for
   * this, and the table is small (one maintainer's hubs, decision 8 of #258).
   */
  async listHubIds(): Promise<string[]> {
    const input: ScanCommandInput = {
      TableName: this.table,
      FilterExpression: 'SK = :meta AND begins_with(PK, :hub)',
      ExpressionAttributeValues: { ':meta': 'META', ':hub': 'HUB#' },
      ProjectionExpression: 'hubId',
    };
    const ids: string[] = [];
    let start: ScanCommandInput['ExclusiveStartKey'];
    do {
      const page = await this.db.send(new ScanCommand({ ...input, ExclusiveStartKey: start }));
      for (const item of page.Items ?? []) ids.push(String(item.hubId));
      start = page.LastEvaluatedKey;
    } while (start);
    return ids.sort();
  }

  async updateHub(hubId: string, telemetry: HubTelemetry, lastSeen: number): Promise<void> {
    await this.updateFields(hubPk(hubId), 'META', { ...telemetry, lastSeen });
  }

  async setHubScheduleConfig(hubId: string, config: ScheduleConfig): Promise<void> {
    await this.updateFields(hubPk(hubId), 'META', { scheduleConfig: config });
  }

  // ---- Cameras -------------------------------------------------------------

  async claimCamera(camId: string, hubId: string) {
    const camera: CameraRecord = { camId, hubId, name: camId, lastSeen: 0 };
    try {
      await this.db.send(
        new TransactWriteCommand({
          TransactItems: [
            {
              Put: {
                TableName: this.table,
                Item: { PK: camPk(camId), SK: 'META', ...camera },
                ConditionExpression: 'attribute_not_exists(PK)',
              },
            },
            {
              Put: {
                TableName: this.table,
                Item: { PK: hubPk(hubId), SK: `CAM#${camId}`, camId },
              },
            },
          ],
        }),
      );
      return { camera, created: true };
    } catch (error) {
      if (!isConditionFailure(error)) throw error;
    }
    const existing = await this.getCamera(camId);
    if (!existing) throw new Error(`Camera ${camId} vanished while claiming it`);
    return { camera: existing, created: false };
  }

  async getCamera(camId: string): Promise<CameraRecord | undefined> {
    return this.getItem<CameraRecord>(camPk(camId), 'META');
  }

  async renameCamera(camId: string, name: string): Promise<void> {
    await this.updateFields(camPk(camId), 'META', { name });
  }

  async updateCamera(camId: string, telemetry: CameraTelemetry, lastSeen: number): Promise<void> {
    await this.updateFields(camPk(camId), 'META', { ...telemetry, lastSeen });
  }

  async listHubCameraIds(hubId: string): Promise<string[]> {
    const items = await this.queryPrefix(hubPk(hubId), 'CAM#');
    return items.map((item) => String(item.camId));
  }

  // ---- Users ---------------------------------------------------------------

  async linkUser(userId: number, hubId: string): Promise<void> {
    await this.db.send(
      new TransactWriteCommand({
        TransactItems: [
          {
            Put: { TableName: this.table, Item: { PK: userPk(userId), SK: `HUB#${hubId}`, hubId } },
          },
          {
            Put: {
              TableName: this.table,
              Item: { PK: hubPk(hubId), SK: `USER#${userId}`, userId },
            },
          },
        ],
      }),
    );
  }

  async listUserHubIds(userId: number): Promise<string[]> {
    const items = await this.queryPrefix(userPk(userId), 'HUB#');
    return items.map((item) => String(item.hubId));
  }

  async isLinked(userId: number, hubId: string): Promise<boolean> {
    return (await this.getItem(userPk(userId), `HUB#${hubId}`)) !== undefined;
  }

  async listHubUserIds(hubId: string): Promise<number[]> {
    const items = await this.queryPrefix(hubPk(hubId), 'USER#');
    return items.map((item) => Number(item.userId));
  }

  async putAppUser(user: AppUser): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: userPk(user.userId), SK: 'APP', ...user },
      }),
    );
  }

  async getAppUser(userId: number): Promise<AppUser | undefined> {
    return this.getItem<AppUser>(userPk(userId), 'APP');
  }

  async getUserSettings(userId: number): Promise<UserSettings | undefined> {
    return this.getItem<UserSettings>(userPk(userId), 'SETTINGS');
  }

  async putUserSettings(userId: number, settings: UserSettings): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: userPk(userId), SK: 'SETTINGS', ...settings },
      }),
    );
  }

  // ---- Pairing codes -------------------------------------------------------

  async putPairingCode(pairing: PairingCode): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        // `ttl` lets DynamoDB delete expired codes; the API checks expiresAt itself.
        Item: { PK: pairPk(pairing.code), SK: 'META', ...pairing, ttl: pairing.expiresAt },
      }),
    );
  }

  async takePairingCode(code: string): Promise<PairingCode | undefined> {
    const { Attributes } = await this.db.send(
      new DeleteCommand({
        TableName: this.table,
        Key: { PK: pairPk(code), SK: 'META' },
        ReturnValues: 'ALL_OLD',
      }),
    );
    if (!Attributes) return undefined;
    const { ttl: _ttl, ...pairing } = strip<PairingCode & { ttl?: number }>(Attributes);
    void _ttl;
    return pairing;
  }

  // ---- Media ---------------------------------------------------------------

  async listMedia(camId: string): Promise<MediaRecord[]> {
    const items = await this.queryPrefix(camPk(camId), 'MEDIA#');
    return items.map((item) => strip<MediaRecord>(item));
  }

  async getMedia(camId: string, mediaId: number): Promise<MediaRecord | undefined> {
    return this.getItem<MediaRecord>(camPk(camId), mediaSk(mediaId));
  }

  async addMedia(camId: string, items: MediaRecord[]): Promise<void> {
    for (const item of items) {
      try {
        await this.db.send(
          new PutCommand({
            TableName: this.table,
            Item: { PK: camPk(camId), SK: mediaSk(item.mediaId), ...item },
            ConditionExpression: 'attribute_not_exists(PK)',
          }),
        );
      } catch (error) {
        if (!isConditionFailure(error)) throw error;
      }
    }
  }

  async removeMedia(camId: string, mediaIds: number[]): Promise<void> {
    for (const mediaId of mediaIds) {
      await this.db.send(
        new DeleteCommand({
          TableName: this.table,
          Key: { PK: camPk(camId), SK: mediaSk(mediaId) },
        }),
      );
    }
  }

  async markThumbPresent(camId: string, mediaId: number): Promise<void> {
    await this.updateIfExists(camPk(camId), mediaSk(mediaId), 'thumbPresent', true);
  }

  async setVideoState(camId: string, mediaId: number, state: VideoState): Promise<void> {
    await this.updateIfExists(camPk(camId), mediaSk(mediaId), 'videoState', state);
  }

  // ---- Commands ------------------------------------------------------------

  async putCommand(camId: string, command: CommandRecord): Promise<void> {
    await this.db.send(
      new PutCommand({
        TableName: this.table,
        Item: { PK: camPk(camId), SK: cmdSk(command.id), ...command },
      }),
    );
  }

  async getCommand(camId: string, id: string): Promise<CommandRecord | undefined> {
    return this.getItem<CommandRecord>(camPk(camId), cmdSk(id));
  }

  async listCommands(camId: string): Promise<CommandRecord[]> {
    const items = await this.queryPrefix(camPk(camId), 'CMD#');
    return items.map((item) => strip<CommandRecord>(item)).sort((a, b) => a.created - b.created);
  }

  // ---- Helpers -------------------------------------------------------------

  private async getItem<T>(pk: string, sk: string): Promise<T | undefined> {
    const { Item } = await this.db.send(
      new GetCommand({ TableName: this.table, Key: { PK: pk, SK: sk } }),
    );
    return Item ? strip<T>(Item) : undefined;
  }

  private async updateFields(pk: string, sk: string, fields: Record<string, unknown>) {
    const entries = Object.entries(fields).filter(([, value]) => value !== undefined);
    await this.db.send(
      new UpdateCommand({
        TableName: this.table,
        Key: { PK: pk, SK: sk },
        ConditionExpression: 'attribute_exists(PK)',
        UpdateExpression: 'SET ' + entries.map((_, i) => `#f${i} = :v${i}`).join(', '),
        ExpressionAttributeNames: Object.fromEntries(entries.map(([key], i) => [`#f${i}`, key])),
        ExpressionAttributeValues: Object.fromEntries(
          entries.map(([, value], i) => [`:v${i}`, value]),
        ),
      }),
    );
  }

  private async updateIfExists(pk: string, sk: string, field: string, value: unknown) {
    try {
      await this.db.send(
        new UpdateCommand({
          TableName: this.table,
          Key: { PK: pk, SK: sk },
          ConditionExpression: 'attribute_exists(PK)',
          UpdateExpression: 'SET #f = :v',
          ExpressionAttributeNames: { '#f': field },
          ExpressionAttributeValues: { ':v': value },
        }),
      );
    } catch (error) {
      if (!isConditionFailure(error)) throw error;
    }
  }

  private async queryPrefix(pk: string, prefix: string): Promise<Record<string, unknown>[]> {
    const input: QueryCommandInput = {
      TableName: this.table,
      KeyConditionExpression: 'PK = :pk AND begins_with(SK, :sk)',
      ExpressionAttributeValues: { ':pk': pk, ':sk': prefix },
    };
    const items: Record<string, unknown>[] = [];
    let start: QueryCommandInput['ExclusiveStartKey'];
    do {
      const page = await this.db.send(new QueryCommand({ ...input, ExclusiveStartKey: start }));
      items.push(...(page.Items ?? []));
      start = page.LastEvaluatedKey;
    } while (start);
    return items;
  }
}
