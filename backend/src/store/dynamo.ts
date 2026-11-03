import { ConditionalCheckFailedException, DynamoDBClient } from '@aws-sdk/client-dynamodb';
import {
  DynamoDBDocumentClient,
  GetCommand,
  PutCommand,
  QueryCommand,
  TransactWriteCommand,
  UpdateCommand,
  type QueryCommandInput,
} from '@aws-sdk/lib-dynamodb';
import type {
  CameraRecord,
  CommandRecord,
  HubRecord,
  MediaRecord,
  ScheduleConfig,
  VideoState,
} from '../model';
import type { CameraTelemetry, HubTelemetry, Store } from './types';

/*
 * Single-table layout (PK / SK):
 *   HUB#<hubId>  / META                   hub record
 *   HUB#<hubId>  / CAM#<camId>            hub → camera (listing)
 *   USER#<id>    / HUB#<hubId>            user ↔ hub link
 *   CAM#<camId>  / META                   camera record (with hubId)
 *   CAM#<camId>  / MEDIA#<dir>#<num>      media (zero-padded, sorts by file)
 *   CAM#<camId>  / CMD#<id>               command
 */
const hubPk = (hubId: string) => `HUB#${hubId}`;
const camPk = (camId: string) => `CAM#${camId}`;
const userPk = (userId: number) => `USER#${userId}`;
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
      new PutCommand({
        TableName: this.table,
        Item: { PK: userPk(userId), SK: `HUB#${hubId}`, hubId },
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
