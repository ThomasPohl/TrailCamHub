# Guide: hub with its own web page (pull)

The hub keeps the pictures on its SD card and serves them from a web page,
for example for someone who **walks past** with a phone, or on a wired
network. Nothing is sent anywhere.

Parts: [shopping list, mode C](shopping-list.md#c-hub-with-its-own-web-page-pull-dongle-or-ethernet-board).
Details: [`modes.md`](../modes.md#pull-the-web-ui), [`installer.md`](../installer.md).

## Where the page is

| Setup | Address | Password |
| --- | --- | --- |
| Own access point (dongles) | join the Wi-Fi `TrailCamHub-XXXX`, open `http://10.42.0.1` | the Wi-Fi password; the page itself asks for none |
| Ethernet (ESP32-S3-ETH, ESP32-P4-NANO) | the DHCP address, or `http://trailcamhub.local` | page password in the boot log and `status`; `config set web_password …` sets your own (after a restart) |
| Home Wi-Fi (dongle with `transport wifi`) | `http://trailcamhub.local` | the access point's WPA2 password, shown on the display |

## Set it up

1. Flash the release for your board with the [web installer](https://thomaspohl.github.io/TrailCamHub/)
   (Chrome or Edge, USB **data** cable).
2. In the setup assistant choose *mains power* and *pull*; for Ethernet or the
   home Wi-Fi also the transport.
3. Power the hub near the camera and press the button (short press = search
   cameras, long press = stop; dongles) or use *Start* on the web page.

## What the page offers

Status of the hub and the cameras, a search/stop button, the live log, a file
browser with thumbnails, photos and videos (videos as MP4, built while you
download), and deletion. While a camera session runs the SD card is busy and
file downloads answer "503"; wait until the search is finished.

## The dongle's display and button

The T-Dongles show the state of the hub on their display; the screens are in
[`docs/images/dongle`](../images/dongle). Short press starts a camera search,
long press stops it.

If something does not work, see [troubleshooting](troubleshooting.md).
