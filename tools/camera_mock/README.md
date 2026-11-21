# Camera mock (ESP32-WROOM)

A cheap ESP32-WROOM-32 that stands in for a TC100 camera, so the hub can be
developed and tested on the bench without the real camera, its battery or the
garden (#122). To the hub it looks like a camera: BLE peripheral, Wi-Fi access
point and the PPPP/ARTEMIS session over UDP.

**Stage 1**: BLE wake-up → AP join → discovery → PRE_LOGIN → login → device
info (512) → media list (768) → heartbeat (525) → CLOSE. **Camera actions**
(part of stage 3): photo (641), video start/stop (643/644), delete (773) and
format (518) are answered. Thumbnails (772), downloads and fault injection
are still open (stages 2 and 3).

## Build and flash

Needs ESP-IDF 5.5 (the CI uses the `espressif/idf:v5.5.6` image). From the
repository root:

```bash
idf.py -C tools/camera_mock -B build-camera-mock set-target esp32
idf.py -C tools/camera_mock -B build-camera-mock build flash monitor
```

The CI job "Build camera mock (ESP32)" also uploads a merged image
(`camera-mock-esp32`), which can be flashed at offset 0:

```bash
esptool.py --chip esp32 write_flash 0 camera-mock.bin
```

Without a toolchain, the web installer's **Camera mock** view
(<https://thomaspohl.github.io/TrailCamHub/#mock>) flashes the same image and
drives the console from the browser: identity, size of the media list, fault
injection, session control ([`docs/installer.md`](../../docs/installer.md)).

Settings (`idf.py -C tools/camera_mock menuconfig`, menu "Camera mock"): the
AP password (default `12345678`), the channel, and whether the AP is up from
boot.

## Test cycle with a hub

1. Power the mock; it advertises as `TC100_0CFB`.
2. On the hub console: `camlist`. The hub wakes the mock over BLE, joins
   `TrailCam_XXXX`, reads device info and the media list and closes the
   session. `scan` gets as far as the thumbnails, which stage 1 does not
   answer.
3. On the mock console: `status` shows the counters of the session.

The hub stores the "camera" under `C6:1E:0D:E0:0C:FB`, the identity of the
captured device info.

## Test with the vendor app

The camera's Android app (TrailCam Go) also connects to the mock. It finds
the mock over BLE, joins its access point and then looks for the camera with
a **LAN search** (`F1 30 00 00`) that it broadcasts to UDP port 32108; the
hub does not do this. The mock answers as the camera in the app's captures
(`trailcam_2-1-connect.pcap`): from port 40611 a `0x41` with the device ID,
and after the app's own `0x41` the camera's address (`0x43`, three times),
the `0x42` and the first DATA (`ACK`). After that the session is the same as
with the hub. `status` counts the LAN searches. Before this the app joined
the access point and then waited for the camera until its timeout.

## Console

Serial console at 115200 baud, prompt `mock>`:

| Command | |
|---|---|
| `status` | BLE, AP and the counters of the UDP session (packets, retransmissions, requests by cmdId) |
| `close` | ends the session from the camera side (CLOSE) |
| `ap on` / `ap off` | turns the access point on or off (off also closes the session) |
| `help` | lists the commands |

## What the mock answers, and from where

Answers are **replayed from real captures**, not built with the hub's code;
otherwise the same mistake on both sides would go unnoticed. Only the
primitives AES-128, Base64 and the parsing of the ARTEMIS request header come
from `components/camera_link`.

| Part | Source |
|---|---|
| Login, 512, 768 pages 0 and 1 | ARTEMIS responses from the pcaps of [pwnb0t/trailcam-automation](https://github.com/pwnb0t/trailcam-automation) (PH970S, firmware 2.3.6.5), still encrypted, in `core/capture_fixtures.cpp` |
| BLE advertisement, notification size, credentials frame | the btsnoop capture of the same repository |
| Discovery reply, first DATA (`ACK`) | our own TC100 captures |
| LAN search answer (`0x41`), camera address (`0x43`) | the app's pcaps of the same repository |
| Fragmentation (1024-byte DATA payloads, header only in the first), PayloadLen = Base64 + 21 | all captured responses |

`core/capture_fixtures.cpp` is generated:

```bash
git clone --depth 1 https://github.com/pwnb0t/trailcam-automation /tmp/trailcam-automation
pip install cryptography
tools/camera_mock/fixtures/extract_fixtures.py /tmp/trailcam-automation/pcap
```

Where the captures say nothing, the mock makes an assumption, marked in the
code:

- **Media-list pages from 2 on** are empty (`cnt` 0, `result` 0), built from
  the documented end-of-list shape (`docs/udp-protocol.md` §5.4); only pages
  0 and 1 were captured. A listing therefore ends after 90 entries.
- **PRE_LOGIN** is decrypted with the bootstrap key and logged, not checked.
  The app never sends one to the camera (#51); the camera's first DATA comes
  on its own 0.4 s after discovery, or at once after a PRE_LOGIN.
- **Commands without login or with another token** than the captured one
  (37817828) are acknowledged but not answered. What the camera does is
  unknown.
- **DATA out of order** is not acknowledged, so the hub repeats it.
- **Camera actions** (641, 643, 644, 773, 518): no capture holds an answer.
  The mock answers with the command's own field (`snapRet`, `recordRet`,
  `delRet`, `formatRet`; 644 only `result`), 0 for success, in the shape of
  the other answers (`docs/camera-actions.md`). A second deletion of the same
  `fileType/dirNum/mediaNum` answers `delRet` -3 ("no such file"); a
  deletion without them -1. After a format the media list is empty. The
  replayed list still shows deleted files and no new photo or video: it is
  a capture. `status` counts the actions.
- Timing: the mock retransmits unacknowledged DATA every 100 ms (the camera
  ~70 ms), sends ALIVE every 2 s and drops a session after 30 s without a
  packet from the hub.

## Limits

The mock only knows what we already understand about the protocol, and it
does not reproduce the camera's radio, timing or throughput. The camera stops
answering when it is commanded before the session has settled
(`docs/udp-protocol.md` §5.2); the mock does not. A test with the real TC100
(#68) stays mandatory before field use.

## Code

- `core/`: pure logic, also built into the host tests
  (`test/host/test_camera_mock.cpp`, which runs the hub's session code against
  the mock).
  - `mock_camera`: session state, reliable DATA/ACK, ARTEMIS commands.
  - `mock_wire`: PPPP framing, written separately from the hub's.
  - `ble_frames`: advertisement, wake-up command, credentials frame.
  - `capture_fixtures`: the captured responses.
- `main/`: ESP-IDF glue: NimBLE peripheral (`mock_ble.c`), access point
  (`mock_ap`), UDP task (`mock_udp`), console (`mock_console`).

## Fault injection (#122 stage 2)

Console command `fault`, shown in `status`:

- `fault drop <0-100>` / `fault dup <0-100>`: drop or duplicate outgoing DATA packets (percent).
- `fault lateack <ms>`: acknowledge the hub's DATA late.
- `fault mute on|off`: answer nothing after the login.
- `fault battery <0-100>|off`, `fault sdfree <MB>|off`: patch the device info (512).
- `fault abort <frames>|off`: stop every bulk record (thumbnail, photo) after that many frames; the rest is never sent.
- `fault off`: all off.

With `drop` the hub waits for a lost fragment instead of taking the later ones first (#313).

## Thumbnails and photos (#122 stages 2 and 3)

`772` (thumbnail) and `1285` (photo download) are answered with `{"cmdRet":0,"result":0,"cmdId":…}` and then one media record on the bulk channel (subtype `0x04` thumbnail, `0x03` photo; 1024-byte frames, one 16-bit sequence for the whole bulk channel, window and retransmission as for DATA). The record is the 20-byte ARTEMIS header, the 72-byte media header (camera MAC, dir, num, length, mediaId from the captured list) and a synthetic JPEG (SOI, filler, EOI): about 3–7 KB for a thumbnail, 40–52 KB for a photo. No capture holds a real one yet. Files that are not in the captured list, deleted or formatted get the answer but no data, as on the real camera. `drop`/`dup` also apply to bulk frames.

## Identity (#122 stage 2)

The camera MAC (`devId` in 512, BLE manufacturer data, media headers) comes from `CONFIG_CAMERA_MOCK_DEV_ID` (default: the captured `C6:1E:0D:E0:0C:FB`) or from the console: `id` shows it, `id C6:1E:0D:E0:0C:42` stores another one in NVS, `id default` goes back to the Kconfig value; both take effect after a reset. The BLE name follows the MAC (`TC100_0C42`). Two mocks with different MACs stand for two cameras (#57).

Thumbnails (772) and photos (1285) are real JPEG files from the flash image: `fixtures/thumbnail.jpg` (320x180, about 4 KB) and `fixtures/photo.jpg` (1280x720, about 23 KB), embedded by the build. They are drawn by `fixtures/make_images.py` (a generic landscape, not from a camera) and the hub's gallery shows them. Every listed recording gets the same picture; without the files (host tests) the mock falls back to a synthetic JPEG frame.

The media list holds 90 recordings, the captured list byte for byte (`CONFIG_CAMERA_MOCK_MEDIA_COUNT`). `media` shows the size, `media <0-1000>` sets it until the next reset: fewer entries cut the captured list, more continue the numbering with invented entries (photo and video alternate, 300 per directory), `media 0` is an empty card. Thumbnails and photos work for every listed entry.

Video playback (769/770): a video of the list is streamed on bulk subtype `0x02` as ARTEMIS version-4 records, one H.264 frame per 40 ms (a key frame with SPS, PPS and IDR every 25 frames) and an AAC frame every second video frame, with the app's page encryption. `totalFrame` follows the list's `durationMs`. The records are made while the bulk queue empties, so a long video never sits in RAM. A photo, a missing or a deleted file gets `startPbRet -3`; `770` stops the stream; `fault abort <frames>` ends it after that many bulk frames. The content is synthetic (no real picture), but the hub's own decoder reads it.
