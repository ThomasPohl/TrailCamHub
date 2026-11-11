# Roadmap

The open work is tracked as [GitHub issues](https://github.com/ThomasPohl/TrailCamHub/issues);
the concept for making TrailCamHub usable by others without running costs is
[#150](https://github.com/ThomasPohl/TrailCamHub/issues/150). This page keeps
the original goals and plan of the project.

## Goals

1. Discover a camera in range and identify its media.
2. Exchange control and connection information over BLE, where the camera
   model supports it.
3. Connect to the camera over Wi-Fi and download new photos and videos.
4. Buffer files locally and persist their upload state.
5. Transfer files over 4G (A7670E modem) or the home network.
6. Resume after interruptions without losing files or uploading duplicates.

TrailCamHub reproduces and automates the workflow of the **TrailCam Go** app
for compatible cameras. The goal is interoperability with a camera the user
owns, not copying or distributing the proprietary app. Protocol observations
are documented reproducibly without credentials, private images or personal
data, and TrailCamHub does not bypass security mechanisms.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Verify board revision, pinout, power supply and modem interface | main board in use; modem test open |
| 2 | Select a camera and document its BLE/Wi-Fi workflow | TC100, see [udp-protocol.md](udp-protocol.md) |
| 3 | BLE-controlled camera Wi-Fi and first API requests | done |
| 4 | Single media download with an integrity check | thumbnails, photos, videos |
| 5 | Persistent queue and resumable uploads | catalog on the SD card |
| 6 | First cloud destination, end-to-end tests | own backend, WebDAV, Telegram |
| 7 | Setup, diagnostics, more cameras, security | in progress |

## Lowering the barrier

Flashing is easy (the web installer); these steps make the rest easier,
roughly in order of impact:

- **Runtime configuration instead of recompiling** — done: modes and
  destinations are set from the installer over Web Serial; one image per
  board serves every user.
- **Camera compatibility first** — publish a compatibility list and a
  "does my camera work?" check as the first step, so an incompatible camera
  shows up at once.
- **Sane defaults** — thumbnails and metadata by default (cheap, winter
  friendly), full photos and videos opt-in.
- **Quick start with a bill of materials and wiring pictures** — removes most
  of the hardware uncertainty.
- **Downlink by polling on wake** — done: the sleeping hub picks up requests
  at each check-in (videos, schedule, Telegram commands) and looks for
  updates itself, instead of keeping a connection open.
- **No own cloud needed** — done: the earlier idea of a copy-paste cloud relay
  (Google Apps Script) gave way to destinations that need no server of ours:
  Telegram directly, WebDAV, or the web UI on the hub (#150).

Realistic target: from "1–3 hours for a tinkerer" to "about 30 minutes for a
technically minded user", with the hard constraints (camera compatibility,
cellular SIM) visible up front.
