# Files on the SD card

What the hub writes to its microSD card (mounted at `/sdcard`), and the
formats of the files other tools may read. The hub never formats its own card (it can format the *camera's* on request, see [camera-actions.md](camera-actions.md)).

```
/sdcard/
  <camera-id>/            one directory per camera
    camera.json           device info of the last session
    catalog.csv           known recordings and what was done with them
    catalog.csv.tmp       only while the catalog is being saved
    thumb/<mediaId>.jpg
    media/<mediaId>.<jpg|mp4|mov|avi|bin|h264|aac>
    media/<mediaId>[.<ext>].part  download in progress
    video_stats.csv       one line per video download
    trace_<mediaId>.csv   only with CONFIG_CAMERA_VIDEO_TRACE
  actions.csv             queue of camera actions (photo, video, delete, format)
  actions.csv.tmp         only while the queue is being saved
  logs/log-NNNNN.txt      rotating log (sd_log)
```

`<camera-id>` is the camera's `devId` with every character outside
`[A-Za-z0-9._-]` replaced by `_`, at most 48 characters
(`sanitize_camera_id()`). Files are named by the `mediaId`, which the camera
never reuses ([media-identity.md](media-identity.md)). A sync covers every
directory that has a `camera.json`.

## `catalog.csv`

Text, one line per recording, written by `MediaCatalog::serialize()`:

```
# trailcamhub-catalog v3
<dir>,<num>,<mediaId>,<fileType>,<mediaTime>,<flags>
```

| Column | Meaning |
|---|---|
| `dir`, `num` | DCIM directory and file number on the camera's card; reused after the card was cleared |
| `mediaId` | identity of the recording (> 0) |
| `fileType` | 0 = photo, 2 = video, as in the media list |
| `mediaTime` | the camera's wall clock as Unix seconds, not UTC ([clock.md](clock.md)) |
| `flags` | decimal bit field, see below |

| Bit | Flag | Meaning |
|---|---|---|
| 0 | `kThumbPresent` | `thumb/<mediaId>.jpg` is stored |
| 1 | `kVideoRequested` | the user asked for the full video |
| 2 | `kFileDownloaded` | the photo or video is in `media/` |
| 3 | `kVideoUploaded` | the video reached the destination |
| 4 | `kReported` | the destination knows the recording |
| 5 | `kVideoUnavailable` | a requested video cannot be fetched |
| 6–7 | attempts | failed downloads of a requested video (given up at 3) |

Versions: v1 was keyed by `dir`/`num`, v2 by `mediaId` with files still named
by `dir`/`num`; both have the same columns and are converted to v3 when
loaded. Rows without a valid `mediaId` and repeated `mediaId`s are dropped. A
file with an unknown header is ignored and the catalog starts empty.

The catalog is saved by writing `catalog.csv.tmp` and renaming it. Whether a
recording is on the camera's card right now (`listed`) is not stored; it is
known only during a session. Entries of recordings that were deleted on the
camera stay in the catalog.

The catalog is read whole into RAM (48 bytes per recording plus the text
while it is parsed). The hub is designed for a few hundred recordings per
camera.

## `camera.json`

One JSON object: the fields of the camera's device info answer (cmdId 512)
that `DeviceInfo` knows (`device_info.cpp`: id, model, firmware, battery, SD
sizes, temperature, charging state, …), plus fields the hub adds:

| Field | Meaning |
|---|---|
| `readAt` | when the device info was read (Unix seconds, 0 = clock not set) |
| `lastSyncAt` | when a session last completed all its work; -1 = completed with an unset clock; absent = never |
| `sdCard`, `sdError` | state of the camera's card from the media list |

## `actions.csv`

The queue of [camera actions](camera-actions.md), replaced through
`actions.csv.tmp` like the catalog. Line 1 is `v1,<next id>`; then one line
per action, newest last:

```
id,type,mediaId,source,state,created,finished,notified,key,reason
7,delete,2653775860,console,done,1794500000,1794500123,1,,
8,photo,0,telegram,failed,1794500100,1794500140,0,tg:4711,the camera answered snapRet -2
```

| Column | Meaning |
|---|---|
| `type` | `photo`, `video-start`, `video-stop`, `delete`, `format` |
| `mediaId` | only for `delete`, else 0 |
| `source` | `console`, `web`, `telegram`, `backend` |
| `state` | `pending`, `running` (sent, no answer yet), `done`, `failed` |
| `created`, `finished` | Unix seconds, 0 = clock not set |
| `notified` | 1 once the front end that asked was told the result |
| `key` | from the front end, recognises a command that arrives twice; `[A-Za-z0-9:_-]` |
| `reason` | why it failed; no commas |

At most 32 lines; the oldest finished one goes first. A `running` line found
at start-up ends as `failed` and is not repeated.

## Who writes

Only one task writes a camera's directory at a time: the camera session task
during a session, the uplink task during the sync that follows (the session
controller never runs both together). The web UI and the console read; the
web UI does not serve videos while a session runs, and its file browser
hides `.part` and `.tmp` files.
