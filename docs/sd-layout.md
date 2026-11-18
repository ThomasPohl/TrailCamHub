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
  found_cameras.csv       cameras heard in BLE scans, read or not (#296)
  approved_cameras.txt    cameras approved in the gallery, one BLE address per line (#299)
  logs/log-NNNNN.txt      rotating log (sd_log)
  logs/cycles.jsonl       one line per cycle (cycle journal)
  logs/cycles.1.jsonl     the previous, full cycle journal
```

`<camera-id>` is the camera's `devId` with every character outside
`[A-Za-z0-9._-]` replaced by `_`, at most 48 characters
(`sanitize_camera_id()`). Files are named by the `mediaId`, which the camera
never reuses ([media-identity.md](media-identity.md)). A sync covers every
directory that has a `camera.json`.

## `catalog.csv`

Text, one line per recording, written by `MediaCatalog::serialize()`:

```
# trailcamhub-catalog v4
<dir>,<num>,<mediaId>,<fileType>,<mediaTime>,<flags>,<durationMs>
```

| Column | Meaning |
|---|---|
| `dir`, `num` | DCIM directory and file number on the camera's card; reused after the card was cleared |
| `mediaId` | identity of the recording (> 0) |
| `fileType` | 0 = photo, 2 = video, as in the media list |
| `mediaTime` | the camera's wall clock as Unix seconds, not UTC ([clock.md](clock.md)) |
| `flags` | decimal bit field, see below |
| `durationMs` | length of a video from the media list, 0 if unknown; over the modem a clearly too long video is refused before the camera read (#160) |

| Bit | Flag | Meaning |
|---|---|---|
| 0 | `kThumbPresent` | `thumb/<mediaId>.jpg` is stored |
| 1 | `kVideoRequested` | the user asked for the full video |
| 2 | `kFileDownloaded` | the photo or video is in `media/` |
| 3 | `kVideoUploaded` | the video reached the destination |
| 4 | `kReported` | the destination knows the recording |
| 5 | `kVideoUnavailable` | a requested video cannot be fetched |
| 6–7 | attempts | failed downloads of a requested video (given up at 3) |
| 8 | `kGone` | not on the camera's card any more (missing from a complete media list, or deleted by the hub) |
| 9 | `kGoneReported` | the backend was told the recording is gone |

Versions: v1 was keyed by `dir`/`num`, v2 by `mediaId` with files still named
by `dir`/`num`; both have the same columns and are converted to v3 when
loaded. v3 is v4 without `durationMs` and is read as is (duration 0). A
firmware from before v4 does not know the v4 header and starts with an empty
catalog after a rollback: thumbnails are fetched and reported again. Rows without a valid `mediaId` and repeated `mediaId`s are dropped. A
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
| `bleAddress` | BLE address the hub connected to (`AA:BB:CC:DD:EE:FF`, #296); kept when the file is rewritten |

## `found_cameras.csv`

Every camera the hub heard advertising in its BLE scans, whether or not it
read it (#296). Written when a cycle's scans are over, sent with every
check-in as `discovered`, so that the user can take cameras in or leave them
out in the gallery (#297, #298). One line per camera, no header:

```
F4:C1:56:06:ED:3B,-55,1800000000,TC100_ED3B
```

| Column | Meaning |
| --- | --- |
| address | BLE address, most significant byte first |
| rssi | signal strength when heard last, dBm |
| seen_at | when heard last (hub clock, Unix seconds; 0 = clock not set) |
| name | advertised name, empty if none was heard |

At most 16 cameras, the most recently heard; a camera not heard for 14 days
is dropped (`camera::found::merge`).

## `approved_cameras.txt`

The BLE addresses of the cameras approved in the gallery (#297, #299), one
per line, from the last check-in whose response carried `approvedCameras`.
When a cycle starts, a hub that syncs with the backend connects only to
these cameras; the others are still heard for `found_cameras.csv`. An empty
file means none (the user stopped reading every camera). No file (never told,
or an old backend), another mode (web UI on the hub, WebDAV), or a camera
directory whose `camera.json` has no `bleAddress` yet (read before the
update): every camera is read.

## `actions.csv`

The queue of [camera actions](camera-actions.md), replaced through
`actions.csv.tmp` like the catalog. Line 1 is `v1,<next id>`; then one line
per action, newest last:

```
id,type,mediaId,source,state,created,finished,notified,key,reason
7,delete,2653775860,console,done,1794500000,1794500123,1,,
8,photo,0,backend,failed,1794500100,1794500140,0,bk:4711,the camera answered snapRet -2
```

| Column | Meaning |
|---|---|
| `type` | `photo`, `video-start`, `video-stop`, `delete`, `format` |
| `mediaId` | only for `delete`, else 0 |
| `source` | `console`, `web`, `backend` |
| `state` | `pending`, `running` (sent, no answer yet), `done`, `failed` |
| `created`, `finished` | Unix seconds, 0 = clock not set |
| `notified` | 1 once the front end that asked was told the result |
| `key` | from the front end, recognises a command that arrives twice; `[A-Za-z0-9:_-]` |
| `reason` | why it failed; no commas |

At most 32 lines; the oldest finished one goes first. A `running` line found
at start-up ends as `failed` and is not repeated.

## `logs/cycles.jsonl`

One JSON object per line, written when a cycle ends (`cycle_trace`), so that
what a hub did can be read without piecing it together from the log:

```
{"cycle":12,"start":1794000000,"kind":"scan","trigger":"schedule","result":"uplink-done","durationS":42,"camerasDone":1,"camerasRead":1,"heapMin":81234,"psramMin":3912345,"stackSession":2100,"stackUplink":5400,"stackController":3300}
{"boot":"panic","time":1794000100,"lastCycle":12}
```

| Field | Meaning |
|---|---|
| `cycle` | number of the cycle; goes on across deep sleep, crashes and restarts (RTC memory) and starts again at 1 after a power loss |
| `start` | Unix seconds when the cycle started; left out while the clock is not set |
| `kind` | `scan` (camera read, then sync), `list`, `actions` or `checkin` (sync only) |
| `trigger` | `schedule`, or `request` (console, button, web UI, backend) |
| `result` | the controller's result, as in the console's `done …` line (`uplink-done`, `camera-unreachable`, …) |
| `durationS` | seconds from leaving idle to the end of the cycle |
| `camerasDone`, `camerasRead` | cameras handled in the cycle and, of those, the ones read |
| `heapMin`, `psramMin` | lowest free heap of the cycle in bytes (internal RAM that data can use, PSRAM; `psramMin` only on boards with PSRAM). On the classic ESP32 about 20 KB of internal heap cannot hold data and are not counted. The free heap is sampled once a second, so a shorter dip only shows when it is the lowest since boot |
| `stackSession`, `stackUplink`, `stackController` | lowest free stack in bytes of the `camera_session`, `uplink` and `session_ctrl` tasks since boot |

A `boot` line is written at every start that is not a wake-up from deep
sleep: the reset reason (`power-on`, `panic`, `task watchdog`, `brownout`, …)
and the last cycle before it, so a cycle without a line of its own is the one
that did not end.

The lines of a cycle in `log-NNNNN.txt` start with its number (`#12 I (5321)
trailcamhub_ctrl: …`), so a log can be cut by cycle. When the file reaches
`CONFIG_SD_LOG_CYCLES_MAX_KB` (256 KB, roughly 800 cycles) it becomes
`cycles.1.jsonl`, replacing the previous one. The web installer's support
package includes the end of the journal.

## Who writes

Only one task writes a camera's directory at a time: the camera session task
during a session, the uplink task during the sync that follows (the session
controller never runs both together). The web UI and the console read; the
web UI does not serve videos while a session runs, and its file browser
hides `.part` and `.tmp` files.
