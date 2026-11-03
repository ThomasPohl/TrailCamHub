import {
  GetObjectCommand,
  HeadObjectCommand,
  PutObjectCommand,
  S3Client,
} from '@aws-sdk/client-s3';
import { getSignedUrl } from '@aws-sdk/s3-request-presigner';

/** Object storage for thumbnails (and later videos). */
export interface MediaStorage {
  /** Short-lived URL the app uses to show a thumbnail. */
  thumbReadUrl(camId: string, mediaId: number): Promise<string>;
  /** Short-lived URL the hub PUTs a JPEG thumbnail to. */
  thumbUploadUrl(camId: string, mediaId: number): Promise<string>;
  thumbExists(camId: string, mediaId: number): Promise<boolean>;
}

export const thumbKey = (camId: string, mediaId: number) => `thumb/${camId}/${mediaId}.jpg`;

const READ_URL_TTL_S = 15 * 60;
const UPLOAD_URL_TTL_S = 30 * 60;

export class S3MediaStorage implements MediaStorage {
  constructor(
    private readonly bucket: string,
    private readonly s3: S3Client = new S3Client({}),
  ) {}

  thumbReadUrl(camId: string, mediaId: number): Promise<string> {
    return getSignedUrl(
      this.s3,
      new GetObjectCommand({ Bucket: this.bucket, Key: thumbKey(camId, mediaId) }),
      { expiresIn: READ_URL_TTL_S },
    );
  }

  thumbUploadUrl(camId: string, mediaId: number): Promise<string> {
    return getSignedUrl(
      this.s3,
      new PutObjectCommand({
        Bucket: this.bucket,
        Key: thumbKey(camId, mediaId),
        ContentType: 'image/jpeg',
      }),
      { expiresIn: UPLOAD_URL_TTL_S },
    );
  }

  async thumbExists(camId: string, mediaId: number): Promise<boolean> {
    try {
      await this.s3.send(
        new HeadObjectCommand({ Bucket: this.bucket, Key: thumbKey(camId, mediaId) }),
      );
      return true;
    } catch (error) {
      const status = (error as { $metadata?: { httpStatusCode?: number } }).$metadata
        ?.httpStatusCode;
      if (status === 404) return false;
      throw error;
    }
  }
}
