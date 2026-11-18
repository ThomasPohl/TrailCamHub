# Guide: hub at home, pictures to your NAS (WebDAV)

A hub in the **home Wi-Fi** next to the camera. After every read it copies new
thumbnails, photos and videos (as MP4) to a folder on your NAS or cloud. No
account with anyone else, no backend.

Parts: [shopping list, mode B](shopping-list.md#b-hub-at-home-with-a-nas-home-wi-fi--webdav).
Settings in detail: [`modes.md`](../modes.md#webdav-webdav).

```
camera ──BLE/Wi-Fi──▶ hub (SD card) ──home Wi-Fi──▶ NAS (WebDAV)
```

## 1. Prepare the NAS

1. Create a folder, for example `TrailCam`, and a user that may write to it
   (an **app password** where the NAS offers one).
2. Find the folder's WebDAV address. Nextcloud shows it under *Files →
   Settings → WebDAV*, for example
   `https://nas.local/remote.php/dav/files/me/TrailCam`.
3. `https://` is checked against the public certificate authorities. A NAS
   with a self-signed certificate needs `http://`, which is unencrypted: use
   it only in a network you trust.

## 2. Flash and set up the hub

1. Open the [web installer](https://thomaspohl.github.io/TrailCamHub/) in
   Chrome or Edge, connect the board with a USB **data** cable and install the
   release for your board.
2. In the setup assistant choose *mains power*, *push* and *home Wi-Fi*, enter
   the Wi-Fi name and password, then choose **WebDAV** and enter URL, user and
   password.
3. Let the assistant check the hub and run the first camera search.

Without the installer, on the serial console:

```
config set transport wifi
config set wifi_ssid MyNetwork
config set wifi_password ...
config set destination webdav
config set webdav_url https://nas.local/remote.php/dav/files/me/TrailCam
config set webdav_user me
config set webdav_password ...
```

## 3. Place it

Put the hub within range of the camera (Bluetooth about 10 m; the camera's own
Wi-Fi is weak) and of your router, with a USB power supply. Walls and
distance cost range; try the first read on the bench.

## 4. What arrives

Each sync creates `<camera-id>/thumb/` and `<camera-id>/media/` in the folder
and copies new files with the camera's date in the name
(`2026-11-08_15-04-05_<mediaId>.jpg`), plus a `camera.json` with the battery
and the SD card state. Over the home network the hub fetches **every** video
(WebDAV cannot ask for one).

A health check keeps an eye on the hub without a server of your own:
`config set health_url https://hc-ping.com/<your-check-uuid>`
([healthchecks.io](https://healthchecks.io) has a free plan) and you get an
e-mail when the hub stops reporting ([modes](../modes.md#health-check)).

If something does not work, see [troubleshooting](troubleshooting.md).
