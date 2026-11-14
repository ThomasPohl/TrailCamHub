# Decisions

Design decisions that shape the code, newest first, each with its reason so
that it can be revisited. Details are in the linked issues and pages. Add an
entry when a decision changes how new code should be written.

## 2026-11-14 — The project keeps the name `trailcamhub_ble_poc`

The menu is now "TrailCamHub" and the options are with their components, but
the ESP-IDF project name stays. It is the `project_name` of the application
description, and a firmware update is refused when it differs from the
running image's (`ota_update.cpp`). A renamed image would be refused by every
hub in the field, which could then only be updated over USB. The name shows
only in build file names (`build/trailcamhub_ble_poc.bin`); the packages and
installer images are named `trailcamhub-<board>-…` already. (#219)

## 2026-11-12 — Camera actions go through one queue, and a lost action is not repeated

Photo, video, delete and format are described once (`camera_action`) and run
from a queue on the SD card, whichever front end asked; the front ends only
translate their input and show the result. Success is the command's own
field, never `result`. An action that was sent and not answered ends as
failed and the person asks again: a repeated delete could remove a newer
recording that took the number. Formatting and deleting are never remote and
need a two-step code. ([camera-actions.md](camera-actions.md), #135, #194)

## 2026-11-11 — One camera per hub on boards without PSRAM, a few hundred recordings per camera

The catalog of a camera is held in RAM and the sync loads every camera on
the card. That is sized for a few hundred recordings per camera. On the
ESP32-WROOM-32D (no PSRAM) one camera per hub is accepted instead of making
the catalog and the sync work in constant memory. (#211)

## 2026-11-11 — The camera mock lives in this repository

`tools/camera_mock/` is its own ESP-IDF project in the same repository, so
that it shares the protocol code with the hub and the host tests run the
hub's parsers against it. (#122)

## 2026-11-07 — Usable by others without running costs for the maintainer

- No shared infrastructure: only free GitHub services (Actions, Releases,
  Pages). A user needs no server or runs their own.
- The AWS backend is optional and is not extended. New functions are built in
  the hub first; nothing may exist only in the backend (label
  `backend-only`).
- The hardware is fixed, the mode is chosen at run time: Kconfig says what a
  board can do (capabilities, `boards.json`), NVS holds the chosen mode
  (power, push/pull, transport, destination). One image per board.
- Telegram directly only for the self-sufficient modem case; in the home
  network the web UI (pull) or WebDAV (push).
- The hub reads `ota.json` straight from the GitHub releases.
- Rejected: a native phone app (a sleeping hub still needs a reachable
  relay; push needs server credentials; store fees) and a backend shared by
  all users (cost).

(#150, [modes.md](modes.md))

## 2026-11-05 — Firmware updates: tagged releases only, installed automatically

Only release builds update, only to a newer version, never to one that was
rolled back; a new firmware confirms itself after its first successful
check-in. (#59, [ota.md](ota.md))

## 2026-11-05 — Recordings are identified by `mediaId`

The camera reuses `dir`/`num` after its card was cleared but never a
`mediaId`; catalog and file names use the `mediaId`. (#85,
[media-identity.md](media-identity.md))

## 2026-11-01 — The camera gets local time as if it were UTC

As the TrailCam Go app does. Times from the camera are therefore wall clock
values, and the camera's time is never given back to it.
([clock.md](clock.md))

## 2026-10-31 — Two use cases, one code base

Autonomous in the garden (battery, modem or home Wi-Fi, deep sleep) and on
site with a person (mains, display, button, web UI) share the camera core
(`camera_ble`, `camera_wifi`, `camera_link`, `sd_storage`). Front ends only
use the request and status interface of the session controller. The camera
core becomes a library of its own only with external users, separate
releases or further camera protocols.

## 2026-10-31 — Deep sleep and a read plan of time windows

The battery hub sleeps between cycles; cameras are read on a list of time
windows, check-ins run on their own interval. (#86,
[power-and-schedule.md](power-and-schedule.md))
