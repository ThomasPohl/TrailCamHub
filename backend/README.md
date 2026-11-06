# TrailCamHub backend (AWS SAM)

A small serverless backend for the hub, the Telegram Mini App and the bot:

```
Hub (ESP32) ──Bearer <hub key>─────┐
Mini App ──tma <Telegram initData>─┼─▶ Lambda (Function URL) ──▶ DynamoDB (hubs, cameras, media, commands, users)
Telegram ──webhook secret token────┘          │        │
                                              │        └─▶ Telegram Bot API (notifications, replies)
                                              └─▶ S3 (thumbnails and videos, via presigned URLs)
```

## Hubs, cameras and users

- A **hub** is one TrailCamHub module. It has its own key and reports for one or
  more trail cameras.
- A **camera** is created automatically the first time a hub reports it and then
  belongs to that hub. If another hub reports the same camera id, that report is
  refused (`accepted: false`), so a hub cannot take over someone else's camera.
- **Users** (Telegram accounts) are linked to hubs and see every camera of their
  hubs. Several users can share a hub. A user is linked by opening a one-time
  **pairing link** (see [Telegram bot](#telegram-bot)) or by an admin
  (`register-hub --user`).

- **One Lambda function** (Node.js 22, TypeScript, arm64) behind a **Function URL**
  — no API Gateway needed.
- **DynamoDB** single table with provisioned 5/5 capacity, which stays in the
  always-free tier.
- **S3** bucket for thumbnails and requested videos. The hub uploads and the app
  downloads directly via short-lived presigned URLs; the bucket stays private.
  Thumbnails expire after `ThumbnailRetentionDays` (365), videos after
  `VideoRetentionDays` (30).
- The **Telegram bot token** is a GitHub secret, passed to the Lambda function at
  deploy time; it never appears in the repository.

Everything is defined in [`template.yaml`](template.yaml) (AWS SAM).

## API v1

| Method | Path                           | Auth                   | Purpose                                        |
| ------ | ------------------------------ | ---------------------- | ---------------------------------------------- |
| GET    | `/v1/health`                   | –                      | Liveness check                                 |
| GET    | `/v1/cameras`                  | `tma <initData>`       | Cameras linked to the Telegram user            |
| GET    | `/v1/cameras/{camId}/media`    | `tma <initData>`       | Recordings with uploaded thumbnails            |
| POST   | `/v1/cameras/{camId}/commands` | `tma <initData>`       | `{"type":"requestVideo","mediaId":3987654321}` |
| POST   | `/v1/cameras/{camId}/commands` | `tma <initData>`       | `{"type":"readCamera"}`                        |
| PUT    | `/v1/hubs/{hubId}/schedule`    | `tma <initData>`       | Wake-up schedule of a hub (see below)          |
| POST   | `/v1/device/checkin`           | `Bearer <hubId>.<key>` | Telemetry + media of all cameras (batch)       |
| POST   | `/v1/device/ack`               | `Bearer <hubId>.<key>` | Confirm uploaded thumbnails and commands       |
| POST   | `/v1/device/pairing-code`      | `Bearer <hubId>.<key>` | One-time pairing link for the hub to show      |
| POST   | `/v1/telegram/webhook`         | webhook secret token   | Updates from Telegram (bot messages, buttons)  |

A recording is identified by its **`mediaId`** (per camera), not by `dir`/`num`:
the camera reuses `dir`/`num` after its card was cleared. `dir`/`num` are only
data. See [docs/media-identity.md](../docs/media-identity.md).

### Hub check-in cycle

One check-in per cycle covers all cameras the hub read, so a cycle needs a single
request (and TLS handshake) however many cameras there are — important over LTE.

1. `POST /v1/device/checkin` with the hub's telemetry and one entry per camera:

   ```json
   {
     "hubFirmware": "0.5.0",
     "hubBatPercent": 91,
     "schedule": {
       "cameraPlan": "05:00-09:00=30,*=120",
       "cameraPlanOrigin": "remote",
       "checkinIntervalS": 1800,
       "checkinOrigin": "built-in",
       "nextCameraReadAt": 1766509500,
       "nextCheckinAt": 1766507700,
       "sleeps": true
     },
     "cameras": [
       {
         "camId": "F4C15606ED3B",
         "readAt": 1766505900,
         "model": "TC100",
         "firmware": "2.3.0.3",
         "batPercent": 78,
         "sdFreeMB": 28412,
         "sdTotalMB": 30436,
         "sdCard": "ok",
         "media": [
           { "mediaId": 4136037234, "dir": 100, "num": 36, "fileType": 0, "mediaTime": 1766505692 }
         ]
       },
       { "camId": "A1B2C3D4E5F6", "batPercent": 40, "media": [] }
     ]
   }
   ```

   - `media` is a **delta**: only recordings the hub has not reported before. The
     backend only adds entries, so repeating one is harmless.
   - `readAt` (hub clock) becomes the camera's "last seen"; implausible values are
     replaced by the server time.
   - `sdCard`: the camera's SD card at the hub's last media list, `ok` or
     `missing` (the camera answered the list with an error, as it does without a
     card); with `missing` also `sdError`, the camera's message (e.g.
     `Memory card not exist`). Absent if the hub does not know; the stored state is then
     kept. `sdFreeMB`/`sdTotalMB` come from the device info (cmdId 512).
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
         "thumbUploads": [{ "mediaId": 4136037234, "url": "https://…" }],
         "commands": [
           {
             "id": "…",
             "type": "requestVideo",
             "mediaId": 3987654321,
             "uploadUrl": "https://…"
           }
         ]
       }
     ],
     "config": { "cameraPlan": "05:00-09:00=30,*=120" }
   }
   ```

   - `schedule` (request): the hub's wake-up schedule in effect and where each
     setting comes from (`built-in` profile default, `local` setting on the hub,
     `remote` from this backend), and the planned next camera read and check-in
     (Unix seconds, absent without a valid clock or if none is planned). Shown in
     the app; implausible values are dropped.
   - `hubBoard`, `hubVariant` (request) and `firmware` (response): a newer firmware
     release for the hub's board and variant, which the hub installs. See
     [docs/ota.md](../docs/ota.md).
   - `config` (response): the schedule set in the app for this hub
     (`cameraPlan`, `checkinIntervalS`); a missing field leaves the setting to the
     hub. See [docs/power-and-schedule.md](../docs/power-and-schedule.md).

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
         "uploadedThumbs": [{ "mediaId": 4136037234 }],
         "commands": [{ "id": "…", "state": "done" }]
       }
     ]
   }
   ```

   The backend checks S3 before marking a thumbnail as present, and only then does
   it appear in the app. Commands are re-sent at every check-in until acknowledged.
   A hub acknowledges `readCamera` at the check-in after it has tried the read:
   `done` if that scan read the camera, `failed` if it did not. It scans (all its
   cameras) right after the check-in that brought the command.

   A `requestVideo` comes with a presigned `uploadUrl` (fresh at every check-in,
   valid for an hour). The hub reads the camera right after the check-in that
   first brought it, downloads the video by playback, and with the next sync
   `PUT`s it as MP4 (`Content-Type: video/mp4`) and acknowledges `done`. It
   acknowledges `failed` with a reason if the video cannot be fetched (no longer
   on the camera, three failed downloads, too large, firmware without video
   downloads). The backend checks S3 before it marks the video `available`; the
   app then gets a `videoUrl` with the recording.

### Hub schedule

`PUT /v1/hubs/{hubId}/schedule` with `{"cameraPlan": "05:00-09:00=30,*=120",
"checkinIntervalS": 900}` replaces the schedule set in the app for a hub the user
is linked to. `null` or a missing field hands that setting back to the hub. The
plan is validated and normalized like on the hub (time windows with an interval
in minutes, `*` for the rest of the day, `off` for no reads); check-in intervals
are 300–86400 seconds. The hub takes the change at its next check-in.

`serverTime` lets the hub set its clock (and the camera's `utcTime`).

## Telegram bot

Besides serving the Mini App, the bot talks to its users. Telegram delivers
updates to `POST /v1/telegram/webhook`; the deploy workflow registers that URL
(`setWebhook`) and the command menu (`setMyCommands`) after every deploy. Every
webhook request carries the header `X-Telegram-Bot-Api-Secret-Token`, derived
from the bot token (`webhookSecret()` in `src/telegram/api.ts`), so nobody else
can post updates. Only private chats are served.

### Notifications

When `/v1/device/ack` confirms a thumbnail for the first time, every user linked
to the hub gets it as a photo with buttons: **🎬 Download video** (videos only;
queues a `requestVideo` like the app) and **📱 Open app** (the Mini App, from the
`MiniAppUrl` parameter). Limits, so a sync never floods a chat:

- only recordings of the last three days (a new camera's backlog stays quiet),
- three photos per camera and check-in, the rest summed up in one message,
- ten messages per user and check-in.

Telegram failures are logged and never fail the hub's request; a user who
blocked the bot is skipped.

### Commands and settings

| Command                   | Effect                                                          |
| ------------------------- | --------------------------------------------------------------- |
| `/start <code>`           | Redeems a pairing code (what opening a pairing link sends)      |
| `/settings`               | Shows the settings, with a button to turn notifications on/off  |
| `/notify on\|off`         | Messages about new recordings                                   |
| `/quiet 22-7` / `off`     | Quiet hours: messages in this window arrive without sound       |
| `/timezone Europe/Berlin` | Time zone of the quiet hours (default: `DefaultTimeZone`)       |
| `/invite`                 | One-time pairing link for one of your hubs, as QR code and text |

Settings are stored per Telegram user (`USER#<id> / SETTINGS`).

### Pairing

A pairing code links whoever redeems it to a hub. It is shared as the link
`https://t.me/<bot>?start=<code>`: opening it (or scanning its QR code) starts
the bot with `/start <code>`, and the bot links the user. Codes work once and
expire (DynamoDB deletes them through the table's TTL); there are three ways to
get one:

| Who                    | How                                               | Valid    |
| ---------------------- | ------------------------------------------------- | -------- |
| Admin                  | `npm run register-hub -- --hub garden-hub --pair` | 7 days   |
| A user linked to a hub | `/invite` in the bot (QR code + link)             | 24 hours |
| The hub                | `POST /v1/device/pairing-code` (to show as QR)    | 30 min   |

The QR code is drawn by the backend itself (`src/qr.ts`), not by a web service.

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
project's home Region (AWS assigns it, e.g. `eu-central-1` or `eu-north-1`;
check AWS Settings → Projects). Calls to other Regions are denied there. Set
the GitHub variable `AWS_REGION` if it is not `eu-central-1`.

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

4. **Register a hub** and get a pairing link for yourself:

   ```bash
   npm run register-hub -- --hub garden-hub --name Garden --pair
   ```

   Open the printed link on your phone: Telegram starts the bot, which links
   your account to the hub. Instead of `--pair` you can also link a Telegram
   user id directly (`--user <id>`, e.g. from
   [@userinfobot](https://t.me/userinfobot)).

   The script runs the registration inside the backend's Lambda function
   (`aws lambda invoke`), because AWS projects deny people direct access to
   DynamoDB data while the function may still use its table. It needs the AWS
   CLI with credentials for the account; `--stack` (default `trailcamhub`) or
   `--function` selects the function.

   The printed hub key goes onto the hub (web installer → Device setup → Device
   key). Cameras need no registration: they appear when the hub first reports
   them. Add family members with `/invite` in the bot, `--pair` or `--user <id>`
   (repeatable); `--rotate-key` issues a new key.

   Links made before the bot sent notifications (before #58) are only stored in
   one direction, and the hub does not know whom to notify. Link those users
   again once: `--user <id>` for each, or a pairing link.

   Admin tasks are only reachable by invoking the function directly; requests
   through the Function URL never trigger them.

## Costs

Lambda, DynamoDB (provisioned 5/5) and outbound traffic stay within the AWS
always-free allowances at this scale. S3 storage is billed normally (cents per
month for thumbnails). In an AWS project the spend limit caps the costs: close to
the limit, AWS blocks new resources and Lambda function URL calls instead of
billing more. In a classic account, set an AWS Budgets alert.

## Not yet included

Video upload (the bot's download button only queues the request), showing the
hub's pairing code as a QR code on the hub, OTA manifest.
