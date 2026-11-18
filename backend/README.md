# TrailCamHub backend (AWS SAM)

A small serverless backend for the hub and the gallery web app:

```
Hub (ESP32) ──Bearer <hub key>────────┐
Gallery app ──Bearer <userId.secret>──┼─▶ Lambda (Function URL) ──▶ DynamoDB (hubs, cameras, media, commands, users)
                                      │          │
                                      │          └─▶ S3 (thumbnails and videos, via presigned URLs)
```

## Hubs, cameras and users

- A **hub** is one TrailCamHub module. It has its own key and reports for one or
  more trail cameras.
- A **camera** is created automatically the first time a hub reports it and then
  belongs to that hub. If another hub reports the same camera id, that report is
  refused (`accepted: false`), so a hub cannot take over someone else's camera.
- **Users** (gallery app users with an app token) are linked to hubs and see
  every camera of their hubs. Several users can share a hub. A user is linked
  by an admin (`register-hub --user <id>`).

- **One Lambda function** (Node.js 22, TypeScript, arm64) behind a **Function URL**
  — no API Gateway needed.
- **DynamoDB** single table with provisioned 5/5 capacity, which stays in the
  always-free tier.
- **S3** bucket for thumbnails and requested videos. The hub uploads and the app
  downloads directly via short-lived presigned URLs; the bucket stays private.
  Thumbnails expire after `ThumbnailRetentionDays` (365), videos after
  `VideoRetentionDays` (30).

Everything is defined in [`template.yaml`](template.yaml) (AWS SAM).

## API v1

| Method | Path                                           | Auth                   | Purpose                                                              |
| ------ | ---------------------------------------------- | ---------------------- | -------------------------------------------------------------------- |
| GET    | `/v1/health`                                   | –                      | Liveness check                                                       |
| GET    | `/v1/cameras`                                  | app user               | Cameras linked to the user                                  |
| GET    | `/v1/me`                                       | app user               | `{userId, name, role, hubs}` of the signed-in user                   |
| GET    | `/v1/admin/hubs`                               | app user, admin        | Every hub: state, cameras, users, faults                             |
| GET    | `/v1/admin/hubs/{hubId}/history`               | app user, admin        | Battery/SD samples of 30 days (#76)                                  |
| POST   | `/v1/hubs/{hubId}/schedule`                    | app user               | Same as the PUT; the apps use it (CORS, #286)                        |
| GET    | `/v1/cameras/{camId}/media`                    | app user               | Recordings with uploaded thumbnails                                  |
| POST   | `/v1/cameras/{camId}/commands`                 | app user               | `{"type":"requestVideo","mediaId":3987654321}`                       |
| POST   | `/v1/cameras/{camId}/commands`                 | app user               | `{"type":"readCamera"}`                                              |
| PUT    | `/v1/hubs/{hubId}/schedule`                    | app user               | Wake-up schedule of a hub (see below)                                |
| POST   | `/v1/cameras/{camId}/commands`                 | app user               | `{"type":"deleteMedia","mediaIds":[…]}`                              |
| POST   | `/v1/cameras/{camId}/media/{mediaId}/withdraw` | app user               | Withdraw queued video/delete requests                                |
| POST   | `/v1/cameras/{camId}/name`                     | app user               | `{"name":"Garten"}`, 1–40 characters                                 |
| POST   | `/v1/cameras/{camId}/hidden`                   | app user               | `{"hidden":true}`: hide in the gallery (#273)                        |
| GET    | `/v1/hubs/{hubId}/found-cameras`               | app user               | Cameras the hub found: new, approved, excluded (#297)                |
| POST   | `/v1/hubs/{hubId}/scan`                        | app user               | Read the cameras at the next contact; the scan finds new ones (#308) |
| POST   | `/v1/hubs/{hubId}/found-cameras/{bleAddress}`  | app user               | `{"state":"approved","name":"…"}` or `excluded`                      |
| POST   | `/v1/device/checkin`                           | `Bearer <hubId>.<key>` | Telemetry + media of all cameras (batch)                             |
| POST   | `/v1/device/ack`                               | `Bearer <hubId>.<key>` | Confirm uploaded thumbnails and commands                             |

A recording is identified by its **`mediaId`** (per camera), not by `dir`/`num`:
the camera reuses `dir`/`num` after its card was cleared. `dir`/`num` are only
data. See [docs/media-identity.md](../docs/media-identity.md).

### Gallery users (#259)

The gallery web app (`app/gallery.html`) signs in with an **app token**,
`Authorization: Bearer <userId>.<secret>`. The admin creates a user and links hubs:

```bash
npm run add-user -- --name "Opa" --hub garden-hub
```

The script prints a set-up link (`…/gallery.html#token=<token>`) to open once
on the user's laptop; the gallery keeps the token. `--user <userId>` issues a
new token (the old link stops working) and can link more hubs;
`--user <userId> --revoke` locks the user out at once (lost laptop) until a
new token is issued. `--admin` sets the admin role: an admin sees and may use
the cameras of **every** hub, linked or not. `GET /v1/me` tells the gallery who
is signed in (`{userId, name, role}`). There is no self-registration.

**Admin view and faults** (#264, #265): `GET /v1/admin/hubs` (admin only)
lists every hub with its reported state and schedule, the schedule set in the
app, its cameras and users, and `faults` from `src/faults.ts`: a hub that is
more than 30 minutes past its planned check-in (or silent for a day without
one) or never checked in, hub battery at most 25 % or 10 % (or a schedule
stretched for it), camera battery at most 25 % or 10 %, a missing camera SD
card, less than 1 GB / 5 % free or less than 200 MB, and failed video or
delete requests of the last 7 days. The gallery shows them to the admin; they
are not sent anywhere yet.

**Battery history** (#76): each check-in stores a sample of the hub battery
and of each camera (battery, SD free, temperature; the camera's at its read
time) as `HUB#<id>` / `CAM#<id>`, `BAT#<at>`, at most one per hour and owner,
deleted by the DynamoDB TTL after half a year. The admin view draws the last
30 days.

**Firmware changes** (#149): when a hub reports another `hubFirmware` than at
its check-in before (update, rollback, flash over USB), the backend stores
`HUB#<id>` / `FW#<at>` with `from` and `to` (ttl half a year).
`/v1/admin/hubs` lists the last five, and a step back to an older version in
the last 7 days is a `firmwareDowngrade` fault. The hub's own update record
(result, reason) is not reported yet.

**Camera names** (#273): a camera is called by its camId until it gets a
name. The end user names his cameras in the gallery ("Umbenennen";
`POST /v1/cameras/{camId}/name`, allowed for every user linked to the
camera's hub). The admin can also set it:
`npm run name-camera -- --camera F4_C1_56_06_ED_3B --name "Garten"`.
The gallery shows the name and uses it for the folder and file names on the
laptop; the camId stays the key. It is a POST because the Function URL's
CORS allows only GET and POST.

**Deleting** (#260): `deleteMedia` queues one command per recording; the hub
gets it as `{"id", "type":"deleteMedia", "mediaId"}` and acknowledges it like
the others. Recordings with a done delete disappear from the media list; the
list shows `deleteState` (`requested`, `delivered`, `failed` with
`deleteError`) and, for failed videos, `videoError`. A queued request can be
withdrawn; once the hub has received it, `withdraw` answers 409. Hub firmware
without delete support acknowledges `failed` ("unsupported command").

### Hub check-in cycle

One check-in per cycle covers all cameras the hub read, so a cycle needs a single
request (and TLS handshake) however many cameras there are — important over LTE.

1. `POST /v1/device/checkin` with the hub's telemetry and one entry per camera:

   ```json
   {
     "hubFirmware": "0.5.0",
     "hubBatPercent": 91,
     "maxThumbUploads": 2,
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
     backend only adds entries, so repeating one is harmless. Recordings that are
     no longer on the camera's card are never in `media`.
   - `removedMedia` (optional): mediaIds no longer on the camera's card (deleted
     on the camera or by a `deleteMedia` command). The backend drops these
     recordings, so the gallery no longer shows them; repeating an id is harmless.
   - `readAt` (hub clock) becomes the camera's "last seen"; implausible values are
     replaced by the server time.
   - `sdCard`: the camera's SD card at the hub's last media list, `ok` or
     `missing` (the camera answered the list with an error, as it does without a
     card); with `missing` also `sdError`, the camera's message (e.g.
     `Memory card not exist`). Absent if the hub does not know; the stored state is then
     kept. `sdFreeMB`/`sdTotalMB` come from the device info (cmdId 512).
   - Optional camera state from the device info, each absent when unknown (the
     stored value is then kept): `temperature` (°C), `isCharging`,
     `isSolarCharging` (booleans), `batType` (the camera's code), `bleVersion`
     and `cameraName` (the name set on the camera). The hub reads only these
     fields of the device info (an allowlist), so nothing else reaches the backend.
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
   - `hubBoard`, `hubVariant` (request): the hub's board and build variant.
     The backend offers no firmware; the hub reads the releases from GitHub
     itself (see [docs/ota.md](../docs/ota.md)).
   - `memory` (request): the hub's memory headroom in the running cycle
     (`cycle`, `heapMin`, and where known `psramMin`, `stackSession`,
     `stackUplink`, `stackController`, all in bytes; see
     [docs/sd-layout.md](../docs/sd-layout.md#logscyclesjsonl)). Sent by the
     hub; this backend ignores it (it is not extended, #150).
   - `config` (response): the schedule set in the app for this hub
     (`cameraPlan`, `checkinIntervalS`); a missing field leaves the setting to the
     hub. See [docs/power-and-schedule.md](../docs/power-and-schedule.md).

   - `thumbUploads`: missing thumbnails, newest first over all cameras, at most 30
     per check-in, each with a presigned `PUT` URL. A hub can ask for fewer with
     `maxThumbUploads` in the request: the response arrives as one TLS record
     (7.6 KB for 4 uploads on the deployed backend), and a hub without PSRAM
     has no block of that size next to the TLS session (#244). The rest follows
     with the next check-ins.
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
an IAM user that may only deploy this stack.

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
   | Variable | `AWS_SAM_ARTIFACT_BUCKET`    | `ArtifactBucketName` output             |
   | Variable | `AWS_CFN_EXECUTION_ROLE_ARN` | `CloudFormationExecutionRoleArn` output |
   | Variable | `AWS_REGION` (optional)      | Home Region if not `eu-central-1`       |

   Then run the **Backend** workflow on `main` (Actions → Backend → Run workflow)
   or push a change under `backend/`. The job summary shows the API URL; set it as
   the variable `TRAILCAMHUB_API_BASE_URL` so the gallery app uses the backend
   instead of demo data.

The access key can only create and run change sets for the `trailcamhub` stack;
everything else happens through the execution role. Rotate it from time to time
(`aws iam create-access-key`, update the secrets, `aws iam delete-access-key`).

4. **Register a hub** and link users to it:

   ```bash
   npm run register-hub -- --hub garden-hub --name Garden --user <userId>
   ```

   `<userId>` is the id of a gallery app user (see [Gallery users](#gallery-users-259)).

   The script runs the registration inside the backend's Lambda function
   (`aws lambda invoke`), because AWS projects deny people direct access to
   DynamoDB data while the function may still use its table. It needs the AWS
   CLI with credentials for the account; `--stack` (default `trailcamhub`) or
   `--function` selects the function.

   The printed hub key goes onto the hub (web installer → Device setup → Device
   key). Cameras need no registration: they appear when the hub first reports
   them. Add family members with `--user <id>`
   (repeatable); `--rotate-key` issues a new key.

   Admin tasks are only reachable by invoking the function directly; requests
   through the Function URL never trigger them.

## Costs

Lambda, DynamoDB (provisioned 5/5) and outbound traffic stay within the AWS
always-free allowances at this scale. S3 storage is billed normally (cents per
month for thumbnails). In an AWS project the spend limit caps the costs: close to
the limit, AWS blocks new resources and Lambda function URL calls instead of
billing more. In a classic account, set an AWS Budgets alert.

## Not yet included

Video upload from the hub and an OTA manifest.

## Found cameras (#296, #297)

A hub with new firmware reports the cameras it heard in its BLE scan
(`discovered`: BLE address, advertised name, RSSI, time) and the BLE address
of every camera it read (`cameras[].bleAddress`). The backend keeps them per
hub as `HUB#<id>` / `BLE#<address>` with a state:

- **new**: heard, not decided yet; the gallery offers to take it in.
- **approved**: the hub may read it. A camera read before any decision is
  approved, so existing cameras keep working after the update.
- **excluded**: the user stopped reading it; a later read does not change
  that, but the user can approve it again at any time.

Every user linked to the hub may decide (`POST …/found-cameras/{address}`).
A name given before the first read becomes the camera's name then. A hub that
reports BLE addresses gets `approvedCameras` (BLE addresses) in the check-in
response; firmware that reads only these follows in #299. Older hubs get no
list and read every camera.
