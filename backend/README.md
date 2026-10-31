# TrailCamHub backend (AWS SAM)

A small serverless backend for the hub and the Telegram Mini App:

```
Hub (ESP32) ──Bearer <hub key>─────┐
                                   ├─▶ Lambda (Function URL) ──▶ DynamoDB (hubs, cameras, media, commands)
Mini App ──tma <Telegram initData>─┘          │
                                              └─▶ S3 (thumbnails, via presigned URLs)
```

## Hubs, cameras and users

- A **hub** is one TrailCamHub module. It has its own key and reports for one or
  more trail cameras.
- A **camera** is created automatically the first time a hub reports it and then
  belongs to that hub. If another hub reports the same camera id, that report is
  refused (`accepted: false`), so a hub cannot take over someone else's camera.
- **Users** (Telegram accounts) are linked to hubs and see every camera of their
  hubs. Several users can share a hub.

- **One Lambda function** (Node.js 22, TypeScript, arm64) behind a **Function URL**
  — no API Gateway needed.
- **DynamoDB** single table with provisioned 5/5 capacity, which stays in the
  always-free tier.
- **S3** bucket for thumbnails. The hub uploads and the app downloads directly via
  short-lived presigned URLs; the bucket stays private.
- The **Telegram bot token** is a GitHub secret, passed to the Lambda function at
  deploy time; it never appears in the repository.

Everything is defined in [`template.yaml`](template.yaml) (AWS SAM).

## API v1

| Method | Path                           | Auth                   | Purpose                                      |
| ------ | ------------------------------ | ---------------------- | -------------------------------------------- |
| GET    | `/v1/health`                   | –                      | Liveness check                               |
| GET    | `/v1/cameras`                  | `tma <initData>`       | Cameras linked to the Telegram user          |
| GET    | `/v1/cameras/{camId}/media`    | `tma <initData>`       | Recordings with uploaded thumbnails          |
| POST   | `/v1/cameras/{camId}/commands` | `tma <initData>`       | `{"type":"requestVideo","dir":100,"num":35}` |
| POST   | `/v1/device/checkin`           | `Bearer <hubId>.<key>` | Telemetry + media of all cameras (batch)     |
| POST   | `/v1/device/ack`               | `Bearer <hubId>.<key>` | Confirm uploaded thumbnails and commands     |

### Hub check-in cycle

One check-in per cycle covers all cameras the hub read, so a cycle needs a single
request (and TLS handshake) however many cameras there are — important over LTE.

1. `POST /v1/device/checkin` with the hub's telemetry and one entry per camera:

   ```json
   {
     "hubFirmware": "0.5.0",
     "hubBatPercent": 91,
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "readAt": 1766505900,
         "model": "TC100",
         "firmware": "2.3.0.3",
         "batPercent": 78,
         "sdFreeMB": 28412,
         "media": [{ "dir": 100, "num": 36, "fileType": 0, "mediaTime": 1766505692 }]
       },
       { "camId": "A1B2C3D4E5F6", "batPercent": 40, "media": [] }
     ]
   }
   ```

   - `media` is a **delta**: only recordings the hub has not reported before. The
     backend only adds entries, so repeating one is harmless.
   - `readAt` (hub clock) becomes the camera's "last seen"; implausible values are
     replaced by the server time.
   - Limits per request: 16 cameras, 2000 media entries.

   The response has one entry per reported camera, plus every other camera of the
   hub that has work (missing thumbnails or commands):

   ```json
   {
     "serverTime": 1766506000,
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "accepted": true,
         "resync": false,
         "thumbUploads": [{ "dir": 100, "num": 36, "url": "https://…" }],
         "commands": [{ "id": "…", "type": "requestVideo", "dir": 100, "num": 35 }]
       }
     ],
     "config": { "sleepIntervalS": 1800 }
   }
   ```

   - `thumbUploads`: missing thumbnails, newest first over all cameras, at most 30
     per check-in, each with a presigned `PUT` URL.
   - `resync: true`: the camera is new to the backend; the hub should send its
     full media list at the next check-in.
   - `accepted: false`: the camera belongs to another hub; nothing was stored.

2. `PUT` each JPEG to its URL (`Content-Type: image/jpeg`).
3. `POST /v1/device/ack`, grouped by camera:

   ```json
   {
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "uploadedThumbs": [{ "dir": 100, "num": 36 }],
         "commands": [{ "id": "…", "state": "done" }]
       }
     ]
   }
   ```

   The backend checks S3 before marking a thumbnail as present, and only then does
   it appear in the app. Commands are re-sent at every check-in until acknowledged.

`serverTime` lets the hub set its clock (and the camera's `utcTime`).

## Development

```bash
cd backend
npm install
npm test             # Vitest, uses an in-memory store
npm run lint && npm run typecheck && npm run format:check
sam validate --lint && sam build
```

## One-time AWS setup

This works in an **AWS project** (the new AWS sign-up, where AWS manages the
organization policies) as well as in a classic account. Projects do not allow
GitHub's OIDC identity provider, so GitHub Actions deploys with the access key of
an IAM user that may only deploy this stack, and the Telegram bot token is a
GitHub secret instead of an SSM parameter.

Prerequisites: AWS CLI with credentials for your project or account, in the
project's home Region (`eu-central-1` for Germany; check AWS Settings →
Projects). Set `AWS_REGION` accordingly if it differs.

1. **Bootstrap** the deployment resources: artifact bucket, CloudFormation
   execution role limited to `trailcamhub-*` resources, and the deploy user:

   ```bash
   aws cloudformation deploy --stack-name trailcamhub-github-deploy \
     --template-file bootstrap/github-deploy.yaml --capabilities CAPABILITY_IAM
   aws cloudformation describe-stacks --stack-name trailcamhub-github-deploy \
     --query 'Stacks[0].Outputs'
   ```

   When `bootstrap/github-deploy.yaml` changes, run the same `deploy` command
   again; GitHub Actions cannot update this stack itself. Outputs, access key and
   repository settings stay the same.

2. **Access key** for the deploy user (`DeployUserName` from the outputs):

   ```bash
   aws iam create-access-key --user-name <DeployUserName>
   ```

3. In GitHub (Settings → Secrets and variables → Actions) set:

   | Kind     | Name                         | Value                                   |
   | -------- | ---------------------------- | --------------------------------------- |
   | Secret   | `AWS_ACCESS_KEY_ID`          | `AccessKeyId` from step 2               |
   | Secret   | `AWS_SECRET_ACCESS_KEY`      | `SecretAccessKey` from step 2           |
   | Secret   | `TELEGRAM_BOT_TOKEN`         | Token from @BotFather                   |
   | Variable | `AWS_SAM_ARTIFACT_BUCKET`    | `ArtifactBucketName` output             |
   | Variable | `AWS_CFN_EXECUTION_ROLE_ARN` | `CloudFormationExecutionRoleArn` output |
   | Variable | `AWS_REGION` (optional)      | Home Region if not `eu-central-1`       |

   Then run the **Backend** workflow on `main` (Actions → Backend → Run workflow)
   or push a change under `backend/`. The job summary shows the API URL; set it as
   the variable `TRAILCAMHUB_API_BASE_URL` so the Mini App uses the backend
   instead of demo data.

The access key can only create and run change sets for the `trailcamhub` stack;
everything else happens through the execution role. Rotate it from time to time
(`aws iam create-access-key`, update the secrets, `aws iam delete-access-key`).
The bot token ends up in a Lambda environment variable, readable by anyone who
can view the function's configuration in your account. In an account where
Parameter Store is available, you can instead deploy with
`TelegramBotTokenParameter=/trailcamhub/telegram-bot-token` and leave the token
parameter empty.

4. **Register a hub** and link it to your Telegram user id (e.g. from
   [@userinfobot](https://t.me/userinfobot)):

   ```bash
   npm run register-hub -- --hub garden-hub --name Garden --user <id>
   ```

   The script runs the registration inside the backend's Lambda function
   (`aws lambda invoke`), because AWS projects deny people direct access to
   DynamoDB data while the function may still use its table. It needs the AWS
   CLI with credentials for the account; `--stack` (default `trailcamhub`) or
   `--function` selects the function.

   The printed hub key goes onto the hub (web installer → Device setup → Device
   key). Cameras need no registration: they appear when the hub first reports
   them. Add family members with `--user <id>` (repeatable); `--rotate-key`
   issues a new key.

   Admin tasks are only reachable by invoking the function directly; requests
   through the Function URL never trigger them.

## Costs

Lambda, DynamoDB (provisioned 5/5) and outbound traffic stay within the AWS
always-free allowances at this scale. S3 storage is billed normally (cents per
month for thumbnails). In an AWS project the spend limit caps the costs: close to
the limit, AWS blocks new resources and Lambda function URL calls instead of
billing more. In a classic account, set an AWS Budgets alert.

## Not yet included

Telegram bot webhook (notifications, buttons), camera pairing by QR code, video
upload, OTA manifest.
