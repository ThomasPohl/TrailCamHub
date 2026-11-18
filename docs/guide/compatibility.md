# Camera compatibility

TrailCamHub talks the protocol of the **TrailCam Go** app: the camera wakes
over Bluetooth LE, opens its own Wi-Fi access point, and answers the app's
commands over UDP ([`udp-protocol.md`](../udp-protocol.md)). A camera that the
TrailCam Go app can use is likely to work; the list below says what has
actually been tried.

| Camera | Seen as | Status | Notes |
| --- | --- | --- | --- |
| TC100 | BLE `TC100_XXXX`, Wi-Fi `TrailCam_XXXX` | **works** on the main board: wake, device info, media list, thumbnails, photos, videos, deleting | the project's own camera (firmware 2.2.5.9 tried) |
| PH970S | as the TC100 | protocol captures from a public repository; the camera mock replays them; not tried with a hub | [`protocol-analysis.md`](../protocol-analysis.md) |
| KJK230 | – | public reference captures only; not tried | [`protocol-analysis.md`](../protocol-analysis.md) |
| Other cameras that work with TrailCam Go | BLE name starts with `TC100_` | unknown, likely | please report |

"Works" means a person ran it on hardware. Everything marked otherwise is an
expectation, not a promise. The hub does not change settings on the camera
except its clock (and only with the options described in
[`clock.md`](../clock.md)).

## Reporting a camera

Open an issue with the *Camera report* template
([new issue](https://github.com/ThomasPohl/TrailCamHub/issues/new/choose)) and
say:

1. Brand, model and firmware version (the TrailCam Go app shows it).
2. The BLE name and the Wi-Fi name the camera shows.
3. The hub board and release you used.
4. How far a `scan` got: found over BLE, Wi-Fi joined, login, media list,
   thumbnails, photo, video. The console's `status` output and the log of the
   run (`/sdcard/logs/`, or the live log in the installer) help most.

Do **not** post passwords, SIM PINs, API keys or the camera's Wi-Fi password.
Captured traffic of a new model is welcome, but remove anything that
identifies you or your place first.
