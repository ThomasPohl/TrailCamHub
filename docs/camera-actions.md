# Camera actions

What the hub can ask a camera to do besides reading it (#135, #194–#196), and
how that works from every front end. The camera commands are described in
[udp-protocol.md](udp-protocol.md) and [camera-media-delete.md](camera-media-delete.md).

| Action | Command | Camera command | Allowed from |
|---|---|---|---|
| `photo` | take a photo | 641 | console, web UI, Telegram, backend |
| `video-start` | start a video | 643 | console, web UI, Telegram, backend |
| `video-stop` | stop it | 644 | console, web UI, Telegram, backend |
| `delete` | delete one recording by `mediaId` | 773 | console, web UI |
| `format` | erase the camera's SD card | 518 | console, web UI |

Formatting is never remote, and neither is deleting (#196). Both need a
confirmation. Changing settings (513) is blocked until a capture of the app
doing it exists (#197); live video (258/259) is parked (#198).

## One path for every front end

```
console / web UI / Telegram ──submit──▶ queue (actions.csv) ──▶ camera session ──▶ camera
                     ◀─────── result (done / failed + reason) ◀──────┘
```

A front end turns its input into an `Action`, checks `allowed()` and, for
`delete` and `format`, the confirmation, and queues it. The camera session
runs the queue. The front ends read the results from the queue, so they all
show the same outcome. The code is `camera_action.cpp` (what an action is,
what the camera's answer means), `action_queue.cpp` (the queue),
`action_confirm.cpp` (the code) and `camera_actions.cpp` (the service with the
SD file); the first three have no ESP-IDF dependency and are host-tested.

### Success is the command's own field

The camera answers `result` 0 even when nothing happened. An action is done
only when its own field says so: `snapRet` (641), `recordRet` (643), `delRet`
(773), `formatRet` (518). 644 has no such field; there `result` 0 counts. An
answer without the field is a failure. A deletion that answers `delRet` -3
says "no such file".

### Confirmation

`delete` and `format` ask for a code and run only when it is repeated within
a minute (once, and the request is dropped after three wrong codes):

```
delete 2653775860            → OK … within a minute send 'delete 2653775860 confirm 4821'
delete 2653775860 confirm 4821
format                       → OK … send 'format confirm 0731'
format confirm 0731
```

The rule is in `ConfirmGate`, so every front end gets the same one.

### Deleting by `mediaId`

`dir/num` are reused by the camera (a new recording takes the highest number
+ 1, also after the highest one was deleted), so a deletion is addressed by
the `mediaId` of the recording (from `camlist`). The session reads the media
list first and looks the `mediaId` up in it right before the command; a file
that is not listed any more is not deleted ("the camera does not list this
file any more"). Success is `delRet` 0; a deleted file is marked as gone in
the catalog, so no thumbnail or download is requested for it.

## When an action runs

- **Short session** (`session_controller_request_actions()`, used by the
  console for everything but `photo`): BLE wake-up, login, device info, the
  actions, CLOSE. No thumbnails, no downloads, no sync, and it does not count
  as a read of the camera for the schedule. The media list is read only if a
  deletion waits.
- **With the next scan** (also what happens when a cycle is already running,
  and for actions from Telegram on a hub on battery): the queued actions run
  right after the device info, so a new photo is in the media list that
  follows, then the deletions with the fresh list, then thumbnails and
  downloads as usual. `photo` on the console starts a scan for that reason.

At the *critical* hub battery level there are no scheduled camera reads
([power-and-schedule.md](power-and-schedule.md)), so a queued action waits
as well.

An action that was sent and not answered (the session ended, the hub
restarted or the camera was silent for 15 s) is **not repeated**: it ends as
`failed` with the reason, and the person asks again. A repeated delete could
remove a newer recording that took the number. An action that waited more than
24 h for a camera read is dropped. At most 8 actions wait, 32 entries are kept.
A command that arrives twice (a Telegram update delivered again after a crash)
is queued once when the front end gives a key such as `tg:<update id>`.

## Console

| Command | |
|---|---|
| `photo` | queue a photo and scan the camera, so the picture is read and pushed |
| `video start` / `video stop` | start or stop a video (a short session) |
| `delete <mediaId>` / `delete <mediaId> confirm <code>` | delete one recording |
| `format` / `format confirm <code>` | erase the camera's SD card |
| `actions` | the queue with the results: `action 3 delete 2653775860 failed: …` |

## Web UI and Telegram

- **Web UI:** the card *Camera actions* has buttons for photo and video, a
  field for the `mediaId` (the file names under `media/` and `thumb/` are the
  `mediaId`) and *Delete recording* and *Erase camera SD card*, which ask for
  confirmation before they go on (`POST /api/action?type=…`, then the same
  request with `&code=` from the first answer). It lists the last actions
  with their results (`GET /api/actions`). The console and the page share
  `session_actions::request()`: checks, confirmation, queue, session.
- **Telegram:** `/photo`, `/video_start`, `/video_stop`. The update id is the
  key, so a command that `getUpdates` delivers again is queued once. The hub
  requests a camera read at once; at the check-in after it, the chat is told
  how each action ended (once). Deleting and formatting are not available in
  a chat. The backend's commands (`CommandType`) are not extended: the
  backend is not extended any more (#150).

## Hardware test points

Not tested on a camera yet (CI compiles only):

- `photo`: the picture is in the media list of the same session; if the new
  photo is missing in the first list, it comes with the next read.
- `video start`, later `video stop`: the video appears; `recordRet` 0.
- `delete <mediaId>` for a file listed by `camlist`: it leaves the list, the
  free space (`status`) rises; a second `delete` of the same id fails with "the
  camera does not list this file any more".
- `format confirm`: the list is empty afterwards; a wrong or late code is
  refused.
- Web UI: each button; the confirmation for delete and format; the list
  under the buttons shows `done` or `failed: …` after the session.
- Telegram `/photo` over the modem: the read starts, the photo arrives with
  the thumbnails of that check-in, and "Photo taken" with the result; the same
  command delivered twice (restart before the offset is saved) takes one
  photo.
- Power off the hub right after sending (`running` in `actions.csv`): after
  the restart the action is `failed`, not repeated.
