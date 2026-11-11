# Camera mock (ESP32-WROOM)

A cheap ESP32-WROOM-32 that stands in for a TC100 camera, so the hub can be
developed and tested on the bench without the real camera, its battery or the
garden (#122). To the hub it looks like a camera: BLE peripheral, Wi-Fi access
point and the PPPP/ARTEMIS session over UDP.

**Stage 1** (this version): BLE wake-up → AP join → discovery → PRE_LOGIN →
login → device info (512) → media list (768) → heartbeat (525) → CLOSE.
Thumbnails (772), downloads and fault injection are stages 2 and 3.

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
