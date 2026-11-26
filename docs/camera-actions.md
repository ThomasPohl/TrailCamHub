# Camera actions

What the hub can ask a camera to do besides reading it (#135, #194–#196), and
how that works from every front end. The camera commands are described in
[udp-protocol.md](udp-protocol.md) and [camera-media-delete.md](camera-media-delete.md).

| Action | Command | Camera command | Allowed from |
|---|---|---|---|
| `photo` | take a photo | 641 | console, web UI, backend |
| `video-start` | start a video | 643 | console, web UI, backend |
| `video-stop` | stop it | 644 | console, web UI, backend |
| `delete` | delete one recording by `mediaId` | 773 | console, web UI, backend |
| `format` | erase the camera's SD card | 518 | console, web UI |
| `settings` | change settings, e.g. `workMode=1;pirSens=2` | 513 | console, web UI, backend (gallery) |

Formatting is never remote (#196); it and deleting need a confirmation.
Settings follow [`camera-settings.md`](camera-settings.md) (#368). Live video
(258/259) is parked (#198).

## One path for every front end

```
console / web UI / backend ──submit──▶ queue (actions.csv) ──▶ camera session ──▶ camera
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
(773), `formatRet` (518). 644 and 513 have no such field; there `result` 0 counts. An
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

### Where a deletion applies (#461)

A deletion has a **scope**: the file on the camera's card, the hub's own
copies (`thumb/<mediaId>.jpg` and every file `media/<mediaId>.*`), or both.
The gallery chooses it per hub (`targets` of the `deleteMedia` command,
[backend README](../backend/README.md)); the console and the web UI delete on
the camera, as before.

| Scope | What happens |
|---|---|
| `camera` | 773 as described above; the hub keeps its files. |
| `camera+hub` | 773 first; only if the camera answers `delRet` 0 the hub's files are deleted. A failed camera delete leaves them. If the hub's files cannot be deleted after the camera's was, the action is `failed` ("deleted on the camera, but …"). A file the camera does not list any more counts as gone: the hub's files are deleted, the answer stays "does not list this file" (done for the person). |
| `hub` | Nothing goes to the camera. It runs in the camera's next session, after the media list (the hub reads the camera for it, as for every deletion), and the camera is not asked. |

The catalog entry gets `kHubDeleted` and loses `kThumbPresent`,
`kFileDownloaded` and `kVideoRequested`. While the camera still lists the
recording, the hub does not fetch its thumbnail or file again. A video that
is asked for later from the gallery clears the flag and is fetched again.
What was reported to the backend (`kReported`, `kVideoUploaded`) stays.

Recordings the **camera lost itself** (deleted there, card formatted) are
found by a complete media list (`mark_unlisted_gone()`); the hub deletes its
copies of them too, whatever the scope of any delete. Not after a list that
failed or was cut off, and not when the camera reports no card. A recording
the hub deleted on the camera only is already marked gone and keeps its
files.

## When an action runs

- **Short session** (`session_controller_request_actions()`, used by the
  console for everything but `photo`): BLE wake-up, login, device info, the
  actions, CLOSE. No thumbnails, no downloads, no sync, and it does not count
  as a read of the camera for the schedule. The media list is read only if a
  deletion waits.
- **With the next scan** (also what happens when a cycle is already running): the queued actions run
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
24 h for a camera read is dropped. At most 40 actions wait, 64 entries are kept
(the gallery deletes in batches, #261).
A command that arrives twice (a backend command delivered again after a crash)
is queued once when the front end gives a key.

## Console

| Command | |
|---|---|
| `photo` | queue a photo and scan the camera, so the picture is read and pushed |
| `video start` / `video stop` | start or stop a video (a short session) |
| `delete <mediaId>` / `delete <mediaId> confirm <code>` | delete one recording |
| `format` / `format confirm <code>` | erase the camera's SD card |
| `actions` | the queue with the results: `action 3 delete 2653775860 failed: …` |
| `camset` | per camera the settings and the capability of its last read: `camset F4_C1_56_06_A1_F0 workMode=0 pirSens=2 … languageCode=en` and `capability F4_C1_56_06_A1_F0 images=1,2,6,10 videos=1,2,4 languages=en,de,fr unsupported=supportSidePir` (#197) |
| `camset <devId\|any> <name=value…>` | queue a `settings` action, e.g. `camset any pirSens=2 osdInfoEnable=1` (flags as 0 or 1); the same path and checks as the web UI |

## Web installer

The *Camera* tab of the installer's advanced view
([installer.md](installer.md)) sends these console commands over USB: buttons
for `photo`, `video start` and `video stop`, the media list of `camlist` as a
table with a *Delete* button per recording, *Erase the camera's SD card* and
the list of `actions`. *Camera settings* (#197) reads `camset` and shows each
camera's settings as a form (choices from the camera's capability, fields it
does not support left out, the detection area left out); *Apply changes*
sends only the changed fields with `camset <devId> name=value …`. Deleting and erasing ask in a dialog first (erasing
wants the word "erase" typed); the page then sends the command, takes the
code from the hub's answer and sends the confirmation.

## Web UI

- **Web UI:** the card *Camera actions* has buttons for photo and video, a
  field for the `mediaId` (the file names under `media/` and `thumb/` are the
  `mediaId`) and *Delete recording* and *Erase camera SD card*, which ask for
  confirmation before they go on (`POST /api/action?type=…`, then the same
  request with `&code=` from the first answer). It lists the last actions
  with their results (`GET /api/actions`). The console and the page share
  `session_actions::request()`: checks, confirmation, queue, session.
- **Backend (gallery, #260, #461):** a `deleteMedia` command (`mediaId`, and
  `targets`: `camera`, `hub`; none means the camera, as an older backend sends
  it) becomes a `delete` with that scope for that camera (an action carries the camera's `devId`, so it
  runs only in that camera's session), keyed `be:<command id>`, so the
  command the backend sends again with every check-in is queued once. The
  user confirmed it in the gallery, so no code is asked for. Queuing it
  requests a camera read at once; the hub answers the command at the check-in
  after the read (usually in the same wake cycle): `done`, also when the
  camera no longer lists the file (it is gone either way), else `failed`
  with the reason. Pure logic in `components/uplink/delete_request.cpp`.
  Formatting stays local only.
- **Web UI (settings, #197):** the page shows the settings the camera reported
  (`settings` and `capability` in `/api/status`) and sends the changed ones as
  a JSON object to `POST /api/settings?cam=<devId>`; the hub reads them with
  `camera::settings::parse_change()` (`session_actions::request_settings()`)
  and queues one `settings` action, like the backend does.
- **Backend (gallery settings, #368):** a `setSettings` command (`settings`
  object) is read with `camera::settings::parse_change()` and becomes a
  `settings` action for that camera, keyed `be:<command id>`, answered like a
  deletion (but every failure is a failure). The session checks the change
  against the device info it just read (range, `capability`, `support…`
  flags) before it sends the 513, adds `seq` as the app does, and after
  `result` 0 writes the new values into `camera.json`, so the check-in after
  the read reports them.

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
- Delete from the gallery with each target (#461): hub + gallery (the hub's
  `thumb/` and `media/` files are gone, the camera's file stays and is not
  fetched again at the next reads); camera + hub; camera only (the hub's files
  stay). A video asked for after a hub-only delete is fetched again. Delete a
  recording by hand on the camera or format its card: the hub's files go with
  it at the next read, and nothing is deleted when the card is missing.
- Web UI: each button; the confirmation for delete and format; the list
  under the buttons shows `done` or `failed: …` after the session.
- Power off the hub right after sending (`running` in `actions.csv`): after
  the restart the action is `failed`, not repeated.
- Settings from the gallery (#368): change e.g. the PIR sensitivity and the
  language; after the hub's next contact the camera's menu and the TrailCam
  Go app show the new values, and the gallery shows them without a pending
  change. A camera that refuses a value: what does it answer to 513?
