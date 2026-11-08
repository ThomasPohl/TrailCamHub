# Modes: power, direction, transport, destination

The hardware of a board is fixed; how it is used is a **mode**, chosen at run
time and stored on the hub (NVS), without another firmware image (concept:
#150). Each setting left empty takes the board's **preset**.

| Setting       | Values                        | Preset (empty)                                    | Console                          |
| ------------- | ----------------------------- | ------------------------------------------------- | -------------------------------- |
| `power`       | `mains`, `battery`            | battery on the S3-A7670E, mains elsewhere          | `config set power battery`       |
| `direction`   | `push`, `pull`                | push on the S3-A7670E, pull elsewhere              | `config set direction push`      |
| `transport`   | `wifi`, `ethernet`, `modem`   | Ethernet if the board has it, else the home Wi-Fi if one is set, otherwise the modem | `config set transport modem` |
| `destination` | `backend`, `webdav`, `telegram` | `backend`                                       | `config set destination webdav`  |

`config clear` (or the installer's "Erase") returns everything to the preset.

- **power** decides deep sleep between cycles and Wi-Fi modem sleep (battery),
  and the default schedule (`docs/power-and-schedule.md`). Battery needs a
  build with deep sleep (`CONFIG_POWER_DEEP_SLEEP`, all boards except the
  ESP32-P4-NANO).
- **direction**: with *push* the hub checks in with its destination after
  reads and on its check-in interval; with *pull* it does not, and people
  fetch thumbnails and media from its web UI. Pull needs a build with the web
  UI; otherwise the direction is push.
- **transport** is the way to the destination: the home Wi-Fi, wired
  Ethernet (ESP32-P4-NANO, ESP32-S3-ETH; the interface is up from boot and
  the sync waits up to the Wi-Fi join timeout for its DHCP address) or the
  cellular modem. The hub's own access point follows with the web UI as a
  station (#150).
- **destination** is where push goes: the own backend (AWS, optional), a
  WebDAV folder, or a Telegram bot directly. See "Destinations" below.

The web installer's setup assistant asks for the mode in plain words and sets
these settings (and the transport's and destination's own settings) for you.

`status` shows the mode that applies and where each part comes from:

```
TCH INFO mode power battery (preset), direction push (setting), transport modem, destination backend; deep sleep between cycles
```

A value the board cannot use is refused when it is set, e.g.
`config set transport modem` on a board without a modem
(`this board has no modem`).

## Destinations

Each destination remembers what it got in the camera's catalog on the SD
card (a thumbnail: `kReported`, a photo or video file: `kVideoUploaded`), so
switching the destination later does not send those items again.

### Own backend (`backend`)

`api_url` and `device_key`; the check-in, the Telegram Mini App and the bot
of the backend (`backend/README.md`). Optional; it is not extended further
(#150).

### WebDAV (`webdav`)

For a NAS or cloud in the home network (Nextcloud, Synology, QNAP, many
routers): `webdav_url` (the folder, `https://` or `http://`), `webdav_user`,
`webdav_password` (HTTP Basic; an app password where the server offers one).

```
config set destination webdav
config set webdav_url https://nas.local/remote.php/dav/files/me/TrailCam
config set webdav_user me
config set webdav_password ...
```

Each sync creates `<camera-id>/thumb/` and `<camera-id>/media/` and copies new
thumbnails, photos and videos (as MP4) with names from the camera's clock
(`2026-11-08_15-04-05_<mediaId>.jpg`), plus `<camera-id>/camera.json` (battery,
SD card). Over the home network the hub fetches **every** video from the
camera for WebDAV, also in builds that otherwise fetch only requested ones;
over the modem only those within the modem's size limit. `https://` checks
the server certificate against the public CAs: a NAS with a self-signed
certificate needs `http://` (unencrypted: only in a network you trust).

### Telegram directly (`telegram`)

Only on a board with a modem (decision in #150; at home use WebDAV or the web
UI). Needs no server:

1. Create a bot with @BotFather and set its token:
   `config set telegram_token 123456789:AA...`, `config set destination telegram`.
2. Send `/start` to the bot. At the next check-in the hub stores this chat as
   the only one it talks to (`telegram_chat`; a group works too) and answers.

At each check-in the hub reads the commands and button presses since the
last one (`getUpdates`), then sends new thumbnails as photos (at most 20 per
check-in, the rest follow) with a **Get video** button under videos, and the
videos that were asked for (as MP4, up to the modem's size limit). Commands:
`/status` (batteries, next read and check-in), `/scan` (read the cameras now),
`/help`.

Limits:

- A sleeping hub answers at its next check-in only; the check-in interval is
  capped at 12 hours with Telegram, since Telegram keeps updates for about a
  day.
- The token is stored on the hub; if the hub is stolen, revoke it with
  @BotFather.
- One bot per hub: a bot with a webhook (the backend's bot) cannot be read
  with `getUpdates` (`409`).
- Compared with the backend's bot there is no Mini App (gallery, camera
  choice, plan card) and no warning when the hub stops reporting.

## Pull: the web UI

With `direction pull` people fetch thumbnails and media from the hub's web UI
(status, search, live log, files, videos as MP4). Where it is served:

- **Ethernet** (ESP32-P4-NANO, ESP32-S3-ETH): at the DHCP address, with the
  page password (boot log).
- **Home Wi-Fi** (boards with the web UI on an access point, e.g. the
  T-Dongle-C5, with `transport wifi` and `wifi_ssid` set): the hub stays
  joined to the home network while idle and serves the page there with the
  page password, which is the access point's WPA2 password (on the display).
  For a camera read it leaves the home network (the radio joins the camera's
  access point) and joins it again afterwards; a lost network is joined again
  every two minutes. Needs mains power.
- **Own access point** `TrailCamHub-XXXX` (http://10.42.0.1), as before; its
  clients need no page password.

Every board with the web UI also answers as **http://trailcamhub.local**
(mDNS; one hub per network under this name).

## Health check

`health_url` is called after every sync: `<url>` after a successful one,
`<url>/fail` after a failed one (the convention of healthchecks.io, which has
a free plan; other services ignore the suffix). When the calls stop — the hub
is broken, has no power or no network — the service warns by e-mail,
Telegram or otherwise. This replaces the backend's warning without a server
of one's own.

```
config set health_url https://hc-ping.com/<your-check-uuid>
```

## Capabilities

What a board can do comes from its build (Kconfig) and is listed per board in
[`boards.json`](../boards.json), which the CI build matrix reads:

| Capability    | Kconfig                         |
| ------------- | ------------------------------- |
| modem         | `CONFIG_MODEM_UPLINK`           |
| ethernet      | `CONFIG_ETH_UPLINK_ENABLE`      |
| battery_gauge | `CONFIG_HUB_BATTERY_MAX17048`   |
| psram         | `CONFIG_SPIRAM`                 |
| display       | `CONFIG_BOARD_TDONGLE`          |
| button        | `CONFIG_BOARD_TDONGLE`          |
| web_ui        | `CONFIG_WEB_UI_ENABLE`          |
| deep_sleep    | `CONFIG_POWER_DEEP_SLEEP`       |

## Boards

| Board                          | Capabilities                              | Preset          |
| ------------------------------ | ----------------------------------------- | --------------- |
| Waveshare ESP32-S3-A7670E-4G   | modem, battery gauge, PSRAM, deep sleep   | battery + push  |
| LilyGO T-Dongle-S3             | display, button, deep sleep               | mains + pull¹   |
| LilyGO T-Dongle-C5             | PSRAM, display, button, web UI, deep sleep| mains + pull    |
| Waveshare ESP32-P4-NANO        | Ethernet (IP101), PSRAM, web UI           | mains + pull    |
| Waveshare ESP32-S3-ETH         | Ethernet (W5500), PSRAM, web UI, deep sleep | mains + pull  |

¹ Without a web UI the direction is push; with no destination set it reads only on request.

### Waveshare ESP32-S3-ETH

Image `s3-eth` (`sdkconfig.defaults.s3-eth`). W5500 on SPI3 (SCLK 13, MOSI 11,
MISO 12, CS 14, INT 10, RST 9), microSD in SPI mode on SPI2 (SCK 7, MOSI 6,
MISO 5, CS 4), console on the native USB-C port. Ethernet is the transport;
the web UI is served on the Ethernet address with a password (both in the
boot log). Power from USB-C or the optional PoE module: the preset is mains.

