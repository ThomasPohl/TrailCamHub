# Modes: power, direction, transport, destination

The hardware of a board is fixed; how it is used is a **mode**, chosen at run
time and stored on the hub (NVS), without another firmware image (concept:
#150). Each setting left empty takes the board's **preset**.

| Setting       | Values                        | Preset (empty)                                    | Console                          |
| ------------- | ----------------------------- | ------------------------------------------------- | -------------------------------- |
| `power`       | `mains`, `battery`, `auto`    | auto on the S3-A7670E (battery if a cell answers), mains elsewhere | `config set power battery` |
| `direction`   | `push`, `pull`                | push on the S3-A7670E, pull elsewhere              | `config set direction push`      |
| `transport`   | `wifi`, `ethernet`, `modem`   | Ethernet if the board has it, else the home Wi-Fi if one is set, otherwise the modem | `config set transport modem` |
| `destination` | `backend`, `webdav`             | `backend`                                       | `config set destination webdav`  |

`config clear` (or the installer's "Erase") returns everything to the preset.

- **power auto** (boards with a battery gauge, #417): the gauge is read at
  boot. A cell that answers means `battery`, none means `mains`. A cell that
  answered before is kept until three boots in a row get no answer, so one I2C
  hiccup does not switch a hub in the field. `status` shows "power battery
  (detected)". A hub on a bench (installer connected) still stays awake for a
  while after each command. A mains-powered hub with a backup cell looks like a
  battery hub: set `power mains` there. Without a gauge `auto` is refused.
- **power** decides deep sleep between cycles and Wi-Fi modem sleep (battery),
  and the default schedule (`docs/power-and-schedule.md`). Battery needs a
  build with deep sleep (`CONFIG_POWER_DEEP_SLEEP`, all boards except the
  ESP32-P4-NANO). On a board with a battery gauge, `power mains` with a cell
  inserted drains it within hours (no deep sleep); `status` and the boot log
  then say "battery detected but power=mains" (#416).
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
  or a WebDAV folder. See "Destinations" below.

## Changing hub settings from the gallery (#197)

The gallery's admin view sets some hub settings through the check-in response
(`config`, see `backend/README.md`). Each setting was checked for whether a
remote change can lock the hub out or leak a secret:

| Setting | From the gallery | Why |
| ------- | ---------------- | --- |
| `camera_plan`, `checkin_min` | yes (kept apart from the hub's own, origin "remote") | the schedule, [`power-and-schedule.md`](power-and-schedule.md) |
| `quiet`, `location` | no (hub only, #436) | the schedule, [`power-and-schedule.md`](power-and-schedule.md) |
| `power` | yes, `powerSetting` | the hub stores it as its own setting if the board can use it (`power auto` needs a gauge, `battery` needs deep sleep; a refused value is only logged) and uses it **from the next start**. A battery hub restarts every cycle; a mains hub keeps its mode until someone restarts it. `battery` on a mains hub puts it to sleep between cycles, so the web UI is then reachable only around the contact times. No value: the hub keeps what it has |
| `direction`, `transport`, `destination`, `wifi_ssid`, `api_url`, `device_key` | no | a wrong value cuts the hub off from the gallery, and nothing can undo it remotely |
| `wifi_password`, `sim_pin`, `apn`, `webdav_*`, `web_password`, `camera_user`, `camera_password` | no | secrets or credentials; they must not pass through the backend |
| `health_url`, `webdav_videos` | no | only for the WebDAV destination, which has no gallery |

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

`api_url` and `device_key`; the check-in and the gallery web app
of the backend (`backend/README.md`). Optional; it is extended as the
gallery needs it (#461).

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
camera for WebDAV, also in builds that otherwise fetch only requested ones
(WebDAV has no way to ask for one). Over the modem it fetches none by
default; `config set webdav_videos all` fetches them there too (videos above
the modem's size limit are fetched but not uploaded), `none` never fetches
videos for WebDAV, `home` is the default. `https://` checks
the server certificate against the public CAs: a NAS with a self-signed
certificate needs `http://` (unencrypted: only in a network you trust).

## Pull: the web UI

With `direction pull` people fetch thumbnails and media from the hub's web UI
(status, search, live log, files, videos as MP4). Where it is served:

- **Ethernet** (ESP32-P4-NANO, ESP32-S3-ETH): at the DHCP address, with the
  page password (boot log and `status`; `config set web_password …` sets one, after a restart).
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
is broken, has no power or no network — the service warns by e-mail
or otherwise. This replaces the backend's warning without a server
of one's own.

```
config set health_url https://hc-ping.com/<your-check-uuid>
```

## Camera login

The hub logs in to the cameras with the factory login `admin`/`admin`. The
TrailCam Go app can change the camera's password (command 514); then set the
new one on the hub, one login for all its cameras:

```
config set camera_password <new password>
config set camera_user <user>        # only if that changed too
```

A rejected login is logged as `camera login rejected (result …); if its
password was changed in the TrailCam Go app, set camera_user/camera_password`.

## Where a setting lives

A setting can come from up to four places. Where more than one has a value,
the one further left in "Wins" applies.

| Setting | Build (Kconfig) | `boards.json` | Hub (NVS, `config set`) | Backend | Wins |
| --- | --- | --- | --- | --- | --- |
| Capabilities (modem, Ethernet, battery gauge, PSRAM, display, web UI, deep sleep) | `CONFIG_MODEM_UPLINK`, … (table below) | `capabilities`: a copy for the installer and the docs; CI fails when it differs from the build | – | – | build |
| Preset of `power` and `direction` | `CONFIG_HUB_PROFILE_*` | `preset`: a copy for the installer | – | – | build |
| `power`, `direction`, `transport`, `destination` | the preset, for an empty setting | – | yes | – | hub, then the preset; a value the board cannot use is refused |
| Camera read plan | `CONFIG_SCHEDULE_CAMERA_PLAN` | – | `camera_plan` | `config` in the check-in response | backend, hub, build ([power-and-schedule.md](power-and-schedule.md)) |
| Check-in interval | `CONFIG_SCHEDULE_CHECKIN_INTERVAL_MIN` | – | `checkin_min` | `config` in the check-in response | backend, hub, build |
| Quiet hours | `CONFIG_SCHEDULE_QUIET` | – | `quiet` | – | hub, build |
| Location (sun times) | `CONFIG_SCHEDULE_LOCATION` | – | `location` | – | hub, build |
| Home Wi-Fi, APN, SIM PIN, destination URLs and credentials, `health_url`, camera login | – | – | yes | – | hub |
| Web UI password | – | – | `web_password`; empty: generated at the first start | – | hub |
| Time zone | `CONFIG_TIME_ZONE` | – | – | – | build |
| Pins, timeouts, limits, video download, log, firmware update policy | the component's `Kconfig` | the board's `defaults` file (`sdkconfig.defaults.<board>`) names the values that differ per board | – | – | build |

Build options are in the `Kconfig` file of the component that uses them
(`menuconfig`: **Component config → TrailCamHub: …**); only the board and the
preset are in `main/Kconfig.projbuild`. The values a board changes are in its
`sdkconfig.defaults.<board>` file, the ones of the release image in
`sdkconfig.defaults.release`.

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
| ESP32-WROOM-32D + SPI microSD  | deep sleep                                | mains + pull¹   |

¹ Without a web UI the direction is push; with no destination set it reads only on request.

### Waveshare ESP32-S3-ETH

Image `s3-eth` (`sdkconfig.defaults.s3-eth`). W5500 on SPI3 (SCLK 13, MOSI 11,
MISO 12, CS 14, INT 10, RST 9), microSD in SPI mode on SPI2 (SCK 7, MOSI 6,
MISO 5, CS 4), console on the native USB-C port. Ethernet is the transport;
the web UI is served on the Ethernet address with a password (both in the
boot log). Power from USB-C or the optional PoE module: the preset is mains.

### ESP32-WROOM-32D with an SPI microSD reader

Wiring diagrams for 38-pin and 30-pin boards: [wiring-wroom32d.md](wiring-wroom32d.md).

Image `wroom32d` (`sdkconfig.defaults.wroom32d`, target `esp32`, #77). A
low-cost classic ESP32 dev board (4 MB flash, no PSRAM) with an external
microSD breakout in SPI mode:

| Breakout | GPIO | Kconfig                   |
| -------- | ---- | ------------------------- |
| SCK      | 18   | `CONFIG_SD_CARD_CLK_GPIO` |
| MOSI     | 23   | `CONFIG_SD_CARD_CMD_GPIO` |
| MISO     | 19   | `CONFIG_SD_CARD_D0_GPIO`  |
| CS       | 5    | `CONFIG_SD_CARD_CS_GPIO`  |
| VCC      | 5 V (VIN) or 3.3 V, see below | |
| GND      | GND  | |

The common blue breakout with an AMS1117 regulator and a 74LVC125A level
shifter ("4.5–5.5 V") must be powered from **5 V** (VIN/USB), not from
3.3 V; its signal lines work with the ESP32's 3.3 V. A breakout without a
regulator takes **3.3 V** (3V3 pin), never 5 V. GPIO5 is a strapping pin and must be high at boot,
which the breakout's pull-up on CS ensures. SDHC cards up to 32 GB, FAT32.

The console is UART0 through the board's USB-to-UART bridge. The 4 MB flash
holds two 1.875 MB OTA slots (`partitions-4mb.csv`). Without PSRAM the hub
reads thumbnails only: no videos (their reorder window needs PSRAM), no
photos and no web UI by default. The regulator and USB bridge of a dev board
keep drawing current in deep sleep, so the preset is mains; with no web UI
the direction is push over the home Wi-Fi.

