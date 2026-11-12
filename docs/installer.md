# Web installer and serial console

**https://thomaspohl.github.io/TrailCamHub/**

The GitHub Pages site flashes the firmware from the browser, sets the hub up
over USB and shows its serial console. No toolchain is needed.

## Requirements

- A desktop Chromium browser with Web Serial (current Chrome or Edge); not
  available in other browsers or on phones.
- A USB **data** cable. The browser asks for permission for each serial port.

## Setup assistant

The page opens with an assistant:

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

Everything else is under **Advanced**: older and development builds, every
single setting, the live protocol view (including the first thumbnail of a
session), the serial console at 115200 baud, a reset over the serial lines, a
command line and a **support package** (status, settings without secrets, the
newest log file from the SD card and the page's serial log in one text file).

The installer does not configure the camera name or the Wi-Fi-enable payload;
the CI build sends `BT_KEY_ON`. Install firmware only from a source you trust
and only on a board you may modify.

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

## Phone app (Telegram Mini App)

With the optional own backend, a Telegram Mini App shows the cameras, their
status and thumbnails and requests videos. It is published on the same site
under **https://thomaspohl.github.io/TrailCamHub/app/**; without a backend it
runs in demo mode with sample data. Code and setup: [`app/`](../app/README.md),
[`backend/`](../backend/README.md).
