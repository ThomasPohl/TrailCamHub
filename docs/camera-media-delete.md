# Deleting media on the camera

Status: tested on hardware on 2026-11-04 with one camera (TC100, firmware 2.2.5.9, 122 GB card). Since #194 the firmware sends 773 and 518 on request ([camera-actions.md](camera-actions.md)): by `mediaId` after a fresh media list, with a confirmation, never from Telegram; 774 is not used. The command IDs come from [`pwnb0t/trailcam-automation`](https://github.com/pwnb0t/trailcam-automation) (`docs/json-commands.md`, `docs/operations-from-apk.md`); everything below was observed on our camera unless it says otherwise.

## Summary

| cmdId | Purpose | Result on 2.2.5.9 |
|---|---|---|
| 773 `DELETE_MEDIA` | delete one file | works |
| 518 `FORMAT_SD_CARD` | erase the whole card | works, in under a second, without any confirmation |
| 774 `DELETE_MEDIA_ALL` | delete all media | **no effect and no answer**; use 518 or repeated 773 |

Three things decide how to use them:

- **`result` is 0 even when nothing was deleted.** Only `delRet` and `formatRet` report the outcome.
- **`dirNum` + `mediaNum` do not identify a file over time.** A number can come back with a new file (see [Number allocation](#number-allocation)), so only `mediaId` identifies a recording, and 773 cannot be addressed by `mediaId`.
- **Nothing can be undone.** The commands need only the session token.

## Delete one file (773)

```json
{"cmdId":773,"fileType":2,"dirNum":100,"mediaNum":1,"token":<number>}
```

Answer on success (all answers came within 20 to 160 ms):

```json
{"delRet":0,"result":0,"cmdId":773}
```

The fields are flat, not in an array as in 772 and 1285. Note the name: the request uses `dirNum` although the media list calls the same value `mediaDirNum`.

The file leaves the media list at once and `sdFreeMB` rises (121678 to 121688 MB for one 15 s video). The deletion lasted over a new session with a new BLE wake-up.

All of these answered `{"delRet":-3,"result":0,"cmdId":773}` and deleted nothing:

| Request | Case |
|---|---|
| `"fileType":2,"dirNum":100,"mediaNum":9999` | number does not exist |
| `"fileType":2,"dirNum":999,"mediaNum":1` | directory does not exist |
| same file as before | already deleted |
| `"fileType":0` or `1` on a video (`fileType` 2 in the list) | wrong type |
| `"fileType":2` on a photo (`fileType` 0 in the list) | wrong type |
| `"fileType":2,"mediaDirNum":100,"mediaNum":2` | `mediaDirNum` instead of `dirNum` |
| `"mediaId":2653775860` | `mediaId` only |
| `"delReqs":[{…}]`, `"deleteReqs":[{…}]`, `"mediaFiles":[{…}]` | array forms (these names are guesses) |
| no fields | |

So `-3` means "no such file for these values", and `fileType` has to be the value from the media list. The value 1 that the app uses for playback (`kVideoAppFileType` in `components/camera_link/playback_policy.hpp`) does not work here. No form for deleting several files with one command was found.

### Together with a playback

- **After 770:** a delete sent directly after the 770 answer (about 350 ms later) succeeded.
- **During a playback:** the camera accepts it (`delRet` 0) and the file leaves the list at once, but frames kept arriving afterwards and 770 was still answered normally. The frames were discarded in this test, so whether the rest of the stream is complete is unknown. Do not rely on it.

### After the deletion

Requests for a deleted file, in a session that stayed open:

| Request | Answer |
|---|---|
| 769 with `fileType` 2 | `{"startPbRet":-5,"result":0,"cmdId":769}` |
| 769 with `fileType` 1 | `{"startPbRet":-3,"result":0,"cmdId":769}` (the same for a file that exists) |
| 1285 | `{"cmdRet":0,"result":0,"cmdId":1285}`, then no data |
| 772 | `{"cmdRet":0,"result":0,"cmdId":772}`, then no data |

1285 and 772 acknowledge a file that is gone, so their answer says nothing about whether it exists.

## Number allocation

A new recording gets **the highest existing number + 1**. Numbers are shared between photos and videos.

- Gaps stay open: with 1, 4 and 5 deleted, new recordings still went to the top.
- Deleting the highest number frees it. `mediaNum` 56 was held by three different recordings during the test:

| Step | `fileType` | `mediaNum` | `mediaId` |
|---|---|---|---|
| original video | 2 | 56 | 1828790833 |
| deleted, then a new photo | 0 | 56 | 3073267824 |
| deleted, then a new photo | 0 | 56 | 3894700905 |
| another photo | 0 | 57 | 425883294 |

- The `mediaId` of all other files did not change.
- After a format the numbers start at 1 again, in directory 100.

This confirms the rule in [`media-identity.md`](media-identity.md) and adds a case: a number is reused not only after the card was cleared, but also after deleting the file with the highest number.

A blind retry of 773 is therefore dangerous: if a new recording of the same type took the number in between, the retry deletes that one. Whether 773 checks a `mediaId` sent along with the other fields was not tested.

## Format the card (518)

```json
{"cmdId":518,"token":<number>}
```

```json
{"errorMsg":"Success","sdTotalMB":122236,"sdFreeMB":122234,"formatRet":0,"result":0,"cmdId":518}
```

- It took 0.9 s, with 54 files on the card. Sent once, it was enough; the reference project sends it several times because the app does.
- The session and the camera's Wi-Fi stay up; heartbeats continue.
- Afterwards the list is empty, also in a new session. The settings in the device info (512) are unchanged: only `timeUTC`, `sdFreeMB` and `batPercent` differed, and `batPercent` also jumped between 60 and 80 without a format.
- A second 518 on an empty card gives the same answer.
- During a playback it also succeeded. 770 was still answered, and the next session and a new photo worked normally.

An empty list is `{"mediaFiles":[],"cnt":0,"pageNo":0,"getMediaListRet":0,"errorMsg":"Success","result":0,"cmdId":768}`.

## Delete all (774)

`{"cmdId":774,"token":<number>}` got no answer within 45 s. The same with `"fileType":0`, `"fileType":2`, `"dirNum":100` and `"fileType":0,"dirNum":100,"mediaNum":0` (8 s each). The list and `sdFreeMB` did not change, and the session stayed up. The ID is only a constant in the app; other fields, or another firmware, may behave differently. The reference project also uses 518 for its `--delete-media-all`.

## The media list after deletions

- The list is sorted by descending number, 45 entries per page. `cnt` is the number of entries **on the page** (45 and 11 for 56 files), not the total; a page past the end is empty with `result` 0.
- Deleting moves older files forward into page 0. The hub reads all pages of the list (up to `CAMERA_MEDIA_LIST_MAX_PAGES`, see #117), so this only changes which page a file is on.
- From reading the code, not tested: entries deleted on the camera stay in the hub catalog with `listed = false` (`MediaCatalog::merge()`), and `missing_thumbnails()` and `missing_downloads()` skip those. A deleted file is thus not requested again, and a reused number cannot be fetched under an old `mediaId`, as long as the list is from the same session.

## If this gets implemented

1. Delete only what is fully stored (and uploaded, if that is the rule), and after 770 for a video.
2. Take a fresh media list directly before deleting, and find the file by `mediaId`. Send `file_type`, `dir_num` and `media_num` from that list entry.
3. Judge the result by `delRet`. If the answer is missing or `-3`, read the list again instead of repeating the delete: the file is gone if its `mediaId` is no longer listed.
4. Use 518 only on an explicit request by a person, and check `formatRet`. A hub that formats by itself could lose recordings it has not fetched yet.
5. Do not use 774.

## How it was tested

A temporary patch of `main` (8c64642, T-Dongle-C5) kept the session open after the media list, sent raw commands from the serial console, logged each answer, and acknowledged and discarded bulk frames without touching the catalog. It was not committed. After each step the media list (all pages) and `sdFreeMB` (512) were read again.

New files came from 641 (trigger snap, answer `{"snapRet":0,"errorMsg":"Success","result":0,"cmdId":641}`) and 643/644 (start and stop recording, answers `{"recordRet":0,…,"cmdId":643}` and `{"errorMsg":"Success","result":0,"cmdId":644}`). The photos have `fileType` 0, the recorded video 2.

Not tested: firmware 2.3.0.3 or other cameras, a camera without a card or with a full one, numbers past directory 100, the mobile app's behaviour, and whether a delete survives cutting the camera's power right after it.
