# Modes: power, direction, transport, destination

The hardware of a board is fixed; how it is used is a **mode**, chosen at run
time and stored on the hub (NVS), without another firmware image (concept:
#150). Each setting left empty takes the board's **preset**.

| Setting       | Values                        | Preset (empty)                                    | Console                          |
| ------------- | ----------------------------- | ------------------------------------------------- | -------------------------------- |
| `power`       | `mains`, `battery`            | battery on the S3-A7670E, mains elsewhere          | `config set power battery`       |
| `direction`   | `push`, `pull`                | push on the S3-A7670E, pull elsewhere              | `config set direction push`      |
| `transport`   | `wifi`, `ethernet`, `modem`   | Ethernet if the board has it, else the home Wi-Fi if one is set, otherwise the modem | `config set transport modem` |
| `destination` | `backend`                     | `backend`                                         | `config set destination backend` |

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
- **destination** is where push goes. Today only the own backend; WebDAV and
  Telegram directly follow (#150).

`status` shows the mode that applies and where each part comes from:

```
TCH INFO mode power battery (preset), direction push (setting), transport modem, destination backend; deep sleep between cycles
```

A value the board cannot use is refused when it is set, e.g.
`config set transport modem` on a board without a modem
(`this board has no modem`).

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

