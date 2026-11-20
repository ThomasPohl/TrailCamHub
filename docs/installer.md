# Web installer and serial console

**https://thomaspohl.github.io/TrailCamHub/**

The GitHub Pages site flashes the firmware from the browser, sets the hub up
over USB and shows its serial console. No toolchain is needed.

## Requirements

- A desktop Chromium browser with Web Serial (current Chrome or Edge); not
  available in other browsers or on phones.
- A USB **data** cable. The browser asks for permission for each serial port.

## Three views

The bar at the top switches between the **Setup assistant**, **Advanced** and
**Camera mock** (see [below](#camera-mock));
each view replaces the other. The view is part of the address (`#wizard`,
`#advanced/console`, `#mock`) and the last one is remembered in the browser. The bar
also shows whether the hub is connected.

### Setup assistant

1. **Choose the board** (from [`boards.json`](../boards.json)).
2. **Install** the release build with [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
3. **Connect** to the hub over the same USB port.
4. **Answer three questions**: where the hub runs (power), how you want to get
   the pictures (push or pull, and where to), how it gets online (transport).
   Only the options the board can do are offered; see [modes.md](modes.md).
5. **Save** — the settings are written to the hub's NVS with the
   [serial commands](#serial-commands) below. Passwords and keys are written
   only, never read back.
6. **Check the hub**: firmware, SD card, battery, settings, then a first
   camera search with the upload.

A finished step can be opened again from the progress bar.

### Advanced

A dock stays in place while the rest scrolls (at the left, in a narrow window
at the top):

- **Hub**: connect and disconnect, the hub's state and its last status (mode,
  schedule, next camera read and check-in, battery, SD card, firmware). The
  status is read again after every scan. See
  [Connecting again](#connecting-again).
- **Commands**: scan, stop, upload, status, reset over the serial lines,
  restart, erase the settings, a command line (arrow up repeats a command) and
  the **support package** (status, settings without secrets, the newest log
  file from the SD card and the page's serial log in one text file).
- **Activity**: what was sent and what the hub reported.

Next to it, five tabs: **Firmware** (older and development builds, manual
download), **Settings** (every single setting, in groups; "Reset" next to a stored
value erases it with `config unset`, after a second click), **Camera** (take
a photo or a video, list the recordings on the camera, delete one, erase the
camera's SD card, and the queue of these actions with their results; see
[camera-actions.md](camera-actions.md)), **SD card** (browse the hub's
SD card, download any file, delete files and directories after a
confirmation; memory view, "check for update", SD write test and every
`status` line as a table; needs a build with
`CONFIG_HUB_CONSOLE_DIAGNOSTICS`), **Protocol** (the
live protocol view, including the first thumbnail of a session) and
**Console** (the serial log at 115200 baud).

### Connecting again

The browser asks for the serial port once. After that the page opens the
hub's port on its own:

- when the connection is lost because the hub went to sleep, was reset or
  unplugged, as soon as the port is back (*Stop waiting* ends that),
- when the page is reloaded while it was connected,
- on **Connect**, if the port used last is there. **Other port…** opens the
  browser's port list instead.

**Disconnect** and the firmware installer close the port until the next
**Connect**. The page remembers only the USB ids of the port
(`trailcamhub.serial.port` in the browser's local storage).

The installer does not configure the camera name or the Wi-Fi-enable payload;
the CI build sends `BT_KEY_ON`. Install firmware only from a source you trust
and only on a board you may modify.

## Camera mock

The **Camera mock** view installs and drives the bench stand-in for the camera
([`tools/camera_mock`](../tools/camera_mock/README.md), #122) on an
**ESP32-WROOM-32D** board without SD card. The mock is not a hub: flash it onto
a second board, never onto the hub.

- **Install**: pick a build and press *Install camera mock* (ESP Web Tools,
  one merged image at offset 0, chip family ESP32). Only builds that carry a
  mock are listed; the *Firmware Package* workflow builds it in the job
  *Build camera mock (ESP32)* and `deploy-installer` publishes it next to the
  boards (`builds/<id>/camera-mock/`, `mock` in `versions.json`). The image is
  also a download with a SHA-256 file.
- **Connect** at the left. The dock shows the camera ID and BLE name, the
  access point, the session counters and the active faults, read with the
  mock's `status`, `id` and `media`. *Close session*, *AP on/off* and *Reset
  board* control it; a command line (arrow up repeats) takes any console
  command.
- **Settings**: the camera MAC (stored in the mock, applies after the reset
  that the page triggers; two mocks with different MACs are two cameras),
  the number of recordings (0–1000, applies at once, back to the build default
  after a reset) and the fault injection (`fault drop|dup|lateack|mute|
  battery|sdfree|abort`, all off with one button; a reset clears them).
- The access point's password, its Wi-Fi channel and starting at boot are
  build options of the mock (`menuconfig` → Camera mock) and cannot be set
  here.
- The **mock console** at the bottom shows the serial log at 115200 baud. The
  hub's keep-alive `ping` is not sent while this view is shown.

## Builds and images

The **Firmware Package** workflow builds every board in `boards.json` with
`"installer": true` (ESP32, ESP32-S3, ESP32-C5 and ESP32-P4 targets) and
publishes the merged images with manifests and checksums after each push to
`main` and for each tag `vX.Y.Z` (once its release is published); a manual
*Run workflow* publishes a branch build. Pull requests build the packages but
never publish. The installer preselects the latest build, which is usually
`main`; a tag build is listed by its version (`v0.1.0 (ebc98a1) —
2026-11-08`), and the newest release is marked. The site keeps the newest 5
releases and the newest 10 other builds. All releases are on the
[releases page](https://github.com/ThomasPohl/TrailCamHub/releases), and the
hub updates itself from there ([ota.md](ota.md)).

Two builds per board: **release** (size-optimised, warnings only) and
**development** (INFO logs including every camera packet).

To flash without the browser, download the `trailcamhub-<board>` artifact of
a workflow run or the merged image from the installer page and write it at
offset 0:

```sh
esptool --chip esp32s3 --port PORT --baud 460800 write-flash 0x0 trailcamhub-esp32s3-dev.bin
```

To enable the site the first time: **Settings → Pages → Source: GitHub
Actions**.

## Serial commands

The console (UART or USB, depending on the board) accepts one command per
line; replies and progress are lines starting with `TCH`. The installer uses
exactly these commands.

| Command | Effect |
|---|---|
| `status` | State, settings (secrets hidden), schedule (next camera read and check-in), web UI address |
| `scan` | Read the camera, then deliver to the destination if one is set |
| `camlist` | List all media on the camera (`TCH EVENT media type=… dir=… num=… id=… time=…`, then `TCH EVENT list done files=… pages=…`); fetches and syncs nothing and does not count as a read for the schedule |
| `photo`, `video start`, `video stop` | Ask the camera to take a photo or start/stop a video ([camera-actions.md](camera-actions.md)) |
| `delete <mediaId>`, `format` | Delete one recording or erase the camera's card; each asks for a code first (`… confirm <code>`) |
| `actions` | The queue of camera actions with the results |
| `upload` | Deliver what is already on the SD card |
| `stop` | End the running scan; a running sync finishes |
| `ping` | Keep a hub on battery awake (the installer sends it while open) |
| `config show` | List the settings |
| `config set <key> <value>` / `config set-hex <key> <hex>` | Store a setting (all keys: `help`; modes and destinations: [modes.md](modes.md)) |
| `config unset <key>` / `config clear` | Erase one or all settings |
| `update` | Sync, then read the firmware manifest now |
| `heap [blocks]`, `ls [path]`, `cat <path> [offset [len]]`, `rm [-r] <path>`, `sdbench` | Diagnostics (only with `CONFIG_HUB_CONSOLE_DIAGNOSTICS`); the SD card tab uses them |

## Gallery web app

With the optional own backend, the gallery web app shows the cameras, their
status and thumbnails, requests videos and deletes recordings. It is published
on the same site under **https://thomaspohl.github.io/TrailCamHub/app/gallery.html**;
without a backend it runs in demo mode with sample data. Code and setup:
[`app/`](../app/README.md), [`backend/`](../backend/README.md).
