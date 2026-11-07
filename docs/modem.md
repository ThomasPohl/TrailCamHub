# Backend uplink over the A7670E modem

The Waveshare ESP32-S3-A7670E-4G has an A7670E LTE Cat-1 modem on the S3's **native USB port**. With `CONFIG_MODEM_UPLINK` (default on that board) the hub syncs with the backend over it instead of the home Wi-Fi: use case 1, a hub in the garden without Wi-Fi.

## Board setup

- **USB DIP switch OFF**: the modem's USB goes to the ESP32-S3 (ON connects it to the USB-C socket instead).
- **4G DIP switch ON**: the modem has power. Between syncs it is put into flight mode (`AT+CFUN=4`).
  Waveshare's FAQ describes switching the power with a GPIO instead (4G DIP switch OFF, GPIO33 high = on). On the ESP32-S3R8 GPIO33–37 belong to the octal PSRAM, so this is not the default: set `CONFIG_MODEM_POWER_GPIO` only once the board revision has been checked.
- **HUB DIP switch**: Waveshare recommends OFF on battery (USB hub circuit without power). Whether the modem's USB path needs the hub on this board has to be checked on hardware.
- The serial console uses UART0 through the CH343 bridge, as before.
- SIM card (nano), LTE antenna. The modem draws peaks of up to about 2 A while it transmits; on a weak 18650 cell this can reset the board (see #138).

## Settings

| Setting     | Values                                                     | Default                                                   |
| ----------- | ---------------------------------------------------------- | --------------------------------------------------------- |
| `transport` | `wifi`, `modem`                                            | the home Wi-Fi if `wifi_ssid` is set, otherwise the modem |
| `apn`       | letters, digits, `.`, `-`, `_` (it goes into `AT+CGDCONT`) | the network's default                                     |
| `sim_pin`   | 4–8 digits (stored like a password, never shown)           | none                                                      |

```
config set transport modem
config set apn internet.telekom
config set sim_pin 1234
config show
```

`config show` ends with `CONFIG backend configured, over modem` (or `incomplete`). The web installer has the same three fields. With the modem only `api_url` and `device_key` are needed for a sync.

## One sync

The session state machine is unchanged: `UPLINK_JOIN` brings the uplink up, `UPLINK` syncs as before, and the end of the cycle takes it down. With the modem (`modem_uplink` component, driver `espressif/iot_usbh_modem` from esp-iot-solution):

1. Power on (GPIO, if set), USB host and modem driver installed.
2. Wait until the modem answers AT commands over USB.
3. Radio on (`AT+CFUN=1`), SIM PIN entered if the card asks for it. A wrong PIN is not tried again (three wrong ones lock the card).
4. Wait for the network registration (home network or roaming).
5. Signal strength and operator are logged, the data call is made (PPP); with an APN the PDP context is set first.
6. The PPP interface gets an IP address and becomes the default route; the sync runs.
7. Afterwards: data call ended, radio off (`AT+CFUN=4`) or power off (GPIO), drivers removed. Deep sleep waits for this (at most 30 s).

Below `CONFIG_HUB_BATTERY_MODEM_MIN_MV` (3400 mV, read right before step 1) the modem is not switched on at all: its current peaks of up to ~2 A could cause a brownout on a nearly empty cell. The cycle then ends like a failed sync. See "Saving power on a low hub battery" in `docs/power-and-schedule.md`.

Steps 1–6 must finish within `CONFIG_MODEM_ATTACH_TIMEOUT_S` (120 s), otherwise the cycle ends with `uplink_unreachable` and the schedule's back-off applies. The log names the step that failed (`Modem uplink failed: …`).

## Not covered yet

- Data volume: requested videos (up to `CONFIG_UPLINK_VIDEO_MAX_MB`, see `docs/video-download.md`) also go over the modem; a lower limit is #143.
- Firmware updates (`docs/ota.md`) go over the modem too: the manifest at most once a day, an update only with enough battery and a signal of at least `CONFIG_OTA_MODEM_MIN_SIGNAL_DBM` (−105 dBm, measured during the attach). The hub restarts into a new firmware only after the modem is down.
- Modem status in the app (#144); GNSS and SMS (#146).
- A USB connection lost during a data call can leave the driver waiting; the next deep sleep resets the hub.
