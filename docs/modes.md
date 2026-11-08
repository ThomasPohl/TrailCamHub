# Modes: power, direction, transport, destination

The hardware of a board is fixed; how it is used is a **mode**, chosen at run
time and stored on the hub (NVS), without another firmware image (concept:
#150). Each setting left empty takes the board's **preset**.

| Setting       | Values                        | Preset (empty)                                    | Console                          |
| ------------- | ----------------------------- | ------------------------------------------------- | -------------------------------- |
| `power`       | `mains`, `battery`            | battery on the S3-A7670E, mains elsewhere          | `config set power battery`       |
| `direction`   | `push`, `pull`                | push on the S3-A7670E, pull elsewhere              | `config set direction push`      |
| `transport`   | `wifi`, `modem`               | the home Wi-Fi if one is set, otherwise the modem  | `config set transport modem`     |
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
- **transport** is the way to the destination. Ethernet and the hub's own
  access point follow (#60, #136).
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
