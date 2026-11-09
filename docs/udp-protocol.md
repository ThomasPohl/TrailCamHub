# UDP protocol reference (PPPP/CS2 + ARTEMIS)

**Updated:** 2026-10-27
**Scope:** General reference for the camera's UDP wire protocol: the reliable-UDP
transport (PPPP/CS2, "Tutk"), the ARTEMIS application layer on top of it, the command
set, and the session lifecycle.

This document describes protocol *structure* only. The encryption of the JSON command
bodies is covered separately in [`protocol-analysis.md`](protocol-analysis.md); this
reference treats the encrypted body as an opaque Base64 blob.

> Facts below are marked **[verified]** when confirmed against our own TC100 captures
> and implementation, and **[tentative]** when taken from reference material and not yet
> independently confirmed.

---

## 1. Protocol stack

```
UDP datagram
└─ PPPP/CS2 reliable-UDP frame      (magic 0xF1, type, length)
   └─ ARTEMIS header                (20 bytes, carried as RUDP DATA payload)
      └─ Base64( AES-128-CBC( JSON command ) ) + 0x00
```

The camera listens on UDP (discovery is observed on port 40611). A single UDP socket
carries the whole session.

---

## 2. PPPP/CS2 frame

Every packet begins with the magic byte `0xF1`, followed by a 1-byte type and a 16-bit
big-endian length. **[verified]**

| Type byte | Name | Meaning |
|---|---|---|
| `0x41` | `DISCOVERY_REQUEST` | LBCS discovery probe |
| `0x42` | `DISCOVERY_REPLY` | discovery answer from the camera |
| `0xD0` | `DATA` | reliable data (carries the ARTEMIS payload) |
| `0xD1` | `DATA_ACK` | acknowledgement of DATA |
| `0xE0` | `ALIVE` | transport keep-alive (from camera) |
| `0xE1` | `ALIVE_ACK` | answer to ALIVE |
| `0xF0` | `CLOSE` | camera ends the session |
| `0xF9` | `PRE_LOGIN` | encryption-init handshake (see §7) |

### 2.1 Control frames
ALIVE / ALIVE_ACK / CLOSE are 4-byte frames with no payload: `F1 <type> 00 00`.
The camera's ALIVE **must** be answered with ALIVE_ACK, or the camera sends CLOSE.
**[verified]**

### 2.2 Reliable DATA/ACK framing ("DRW")
DATA and ACK use an 8-byte header: **[verified]**

```
F1 <D0|D1> <length:2 BE> D1 <channel=0> <word:2 BE> <payload...>
          └ length = bytes after the length field (header remainder + payload)
```

- For **DATA** (`0xD0`), `word` is the per-session packet **index**.
- For **ACK** (`0xD1`), `word` is the **count** of acknowledged indices, which follow
  as the payload. A single-index ACK is `F1 D1 00 06 D1 00 00 01 <index:2>`.

### 2.3 Discovery (LBCS)
A fixed 24-byte probe; the camera replies with a `0x42` frame. **[verified]**

```
F1 41 00 14 4C 42 43 53 00 00 00 00 00 00 00 00 43 43 43 4A 4A 00 00 00
            └ "LBCS"                             └ "CCCJJ"
```

**Origin of the constants (#51).** Checked against all 23 pcaps of
[pwnb0t/trailcam-automation](https://github.com/pwnb0t/trailcam-automation)
(TrailCam Go app 2.5.6 and a Python script, a MAXDONE camera of the same family):

- **Device ID.** The probe is byte-for-byte the same in all 96 discovery requests. It is
  the CS2/PPPP device ID `LBCS-000000-CCCJJ`, and the camera itself sends `0x41` and
  `0x43` packets carrying the same ID to the phone. The MAXDONE camera and our TC100
  have the same ID, so it is a fixed ID of this camera family, not a serial number.
- **PRE_LOGIN (`0xF9`).** The app sends no `0xF9` to the camera at all. It sends one
  `0xF9` of 84 bytes to each of three CS2 P2P servers (`119.23.219.190`,
  `47.52.46.203`, `121.43.173.0`, UDP port 32100); its first 23 bytes are always
  `0c cb 9a 2b … c9 d1 aa`, the next five differ per app session. The hub's 28-byte
  "static header" is the start of one of these server packets, taken over by
  pi_trailcam. In the captures the camera sends its first DATA packet (`F1 D0 00 07
  D1 00 00 00 "ACK"`) about 0.3–0.5 s after discovery on its own, without any
  PRE_LOGIN. The hub still sends PRE_LOGIN by default, because that is the flow verified
  on hardware; `CONFIG_CAMERA_SEND_PRELOGIN` switches it off for a test.

---

## 3. Reliability layer

The transport is a sliding-window reliable-UDP scheme implemented in
`components/camera_link/pppp_session`. **[verified]**

- DATA packets are numbered per session; indices restart at 0 on `reset()` in both
  directions.
- Up to **4** DATA packets may be outstanding (unacknowledged) at once.
- Each unacknowledged DATA is **retransmitted every 800 ms** until its ACK arrives.
- Received DATA is acknowledged immediately; duplicates (index not ahead of the next
  expected index, using a wrap-around `< 0x8000` comparison) are re-acknowledged but
  not re-delivered.
- ALIVE is answered with ALIVE_ACK; CLOSE and repeated discovery replies are reported
  to the caller.

This layer is free of ESP-IDF dependencies and is unit-tested on the host.

---

## 4. ARTEMIS application layer

An ARTEMIS message is the payload of a DATA frame. Its 20-byte header is: **[verified]**

| Offset | Field | Size | Encoding | Value |
|---|---|---|---|---|
| 0x00 | Signature | 8 | ASCII | `"ARTEMIS\0"` (`41 52 54 45 4D 49 53 00`) |
| 0x08 | MsgType | 4 | uint32 LE | `2` = request, `3` = response |
| 0x0C | AppSeq | 4 | uint32 LE | application sequence counter |
| 0x10 | PayloadLen | 4 | uint32 LE | bytes that follow: Base64 + NUL + zero padding |

After the header comes a **Base64** string terminated by `\x00` and then zero-padded;
`PayloadLen` counts all of it, so the Base64 ends at the first NUL. It decodes (as
**AES-128-CBC**, zero IV) to a NUL-terminated JSON object. The key and cipher parameters
are documented in [`protocol-analysis.md`](protocol-analysis.md).

---

## 5. Command set

Commands are JSON objects distinguished by `cmdId`. After login, every command carries
the numeric session `token` returned by the login response. **[verified]** (command
IDs and shapes confirmed against captured app logs)

| cmdId | Name | Direction | Notes |
|---|---|---|---|
| 0 | LOGIN | C→D | once per session |
| 258 | START_AV | C→D | start audio/video stream |
| 259 | STOP_AV | C→D | stop stream |
| 512 | GET_DEV_INFO | C→D | device/firmware/battery/SD info |
| 518 | FORMAT_SD_CARD | C→D | erases the card (seen on firmware 2.2.5.9), see [`camera-media-delete.md`](camera-media-delete.md) |
| 525 | HEARTBEAT | C→D | keep-alive, ~every 3 s, fire-and-forget |
| 641 | TRIGGER_SNAP | C→D | takes a photo (seen on firmware 2.2.5.9) |
| 643 / 644 | TRIGGER_RECORD / STOP_RECORD | C→D | starts / stops a video (seen on firmware 2.2.5.9) |
| 768 | GET_MEDIA_LIST | C→D | list stored media |
| 772 | GET_THUMBNAIL | C→D | media thumbnails |
| 773 | DELETE_MEDIA | C→D | deletes one file (seen on firmware 2.2.5.9), see [`camera-media-delete.md`](camera-media-delete.md) |
| 774 | DELETE_MEDIA_ALL | C→D | no effect and no answer on firmware 2.2.5.9 |

The login, device-info and media-list exchanges below are **[verified on hardware]**
(TC100, firmware 2.3.0.3, model "Wi-Fi TrailCam" / `LUTRANSTRA`).

### 5.1 Login (cmdId 0)
Request (decrypted):
```json
{"cmdId":0,"usrName":"admin","password":"admin","needVideo":0,"needAudio":0,"utcTime":<unix>,"supportHeartBeat":true}
```
Response:
```json
{"cmdId":0,"result":0,"token":<number>,"bat_percent":<n>,"errorMsg":"Success"}
```
`result:0` means success; the numeric `token` must be included in every later command.

### 5.2 Command ordering (important)
The camera is sensitive to *when* and *how* commands are sent:
- Send the first command only after the session has **settled** — in practice after a
  heartbeat has been acknowledged. A command fired immediately on the login response (while
  the camera is still retransmitting it) makes the camera stop responding.
- As the app does, request **device info (512) first**, then the media list (768).
- Commands carry the token: `{"cmdId":<id>,...,"token":<number>}`.

### 5.3 Device info (cmdId 512)
Request: `{"cmdId":512,"token":<number>}`. The response is a large JSON object (a few KB,
fragmented — see §5.6) with device identity and state: `devId`, `fwVerName`, `modelName`,
`subModelName`, `productId`, `batPercent`, `sdTotalMB`, `sdFreeMB`, a `capability` object,
`result:0`, etc.

### 5.4 Media list (cmdId 768)
Request — the paging parameters are **required**, or the camera does not answer:
```json
{"cmdId":768,"itemCntPerPage":45,"pageNo":0,"token":<number>}
```
Response:
```json
{"mediaFiles":[{"fileType":0,"mediaDirNum":100,"mediaNum":36,"durationMs":0,
                "mediaId":4136037234,"mediaTime":1766505692}, …],
 "cnt":<n>,"pageNo":0,"getMediaListRet":0,"errorMsg":"Success","result":0,"cmdId":768}
```
Per entry: `fileType` (0 and 2 appear paired per capture — photo/video), `mediaDirNum`
+ `mediaNum` address the file (DCIM dir 100), `mediaId` is a 32-bit id for later
download/thumbnail, `mediaTime` is a Unix timestamp (unreliable while the camera clock
is unset). Page with `pageNo`.

Paging: the list is sorted newest first (descending `mediaNum`). A page holds up to
`itemCntPerPage` (45) entries; `cnt` is the number of entries on that page, not a total,
and no total is reported. A page past the end is empty with `result` 0, so the list
ends with a page of fewer than 45 entries. A scan reads the pages in order until then,
or until `CAMERA_MEDIA_LIST_MAX_PAGES` (default 20) pages were read, and merges each
page into the catalog; thumbnails and downloads are then fetched newest first.

### 5.5 Heartbeat (cmdId 525)
Minimal body `{"cmdId":525}`, sent roughly every 3 seconds. The application sequence for
heartbeats starts at `0x10001`. The firmware currently replays a precomputed heartbeat
frame rather than encrypting one at runtime.

### 5.6 Large responses (fragmentation) **[verified on hardware]**
Large results (device info, media list) are split across several DRW DATA frames of
~1 KB with consecutive indices. Only the **first** fragment carries the 20-byte ARTEMIS
header; the rest are raw continuation. Concatenating the DATA payloads and reading the
header's `PayloadLen` (total Base64 + NUL + padding) reassembles the whole message, which
is then decoded as one unit. Each fragment is individually acknowledged. A datagram can be
~1.6 KB, so the receive path must not assume a small MTU.

---

## 6. Session lifecycle

Driven by the session state machine
(`components/session_controller/session_fsm`): **[verified]**

```
idle → ble_activation → wifi_join → session → (backoff → …)
```

Within the UDP session (**verified on hardware**):

```
Discovery (LBCS) → PRE_LOGIN (0xF9, probably not needed, §2.3) → camera "ACK" DATA → ARTEMIS login (cmdId 0)
→ Magic1/Magic2 → heartbeats → login response (token)
→ [settle: wait for a heartbeat ACK] → device info (512) → media list (768)
```

---

## 7. Login handshake and timing

This flow is **verified on hardware** (the earlier belief, from reference captures, that
the genuine client skips the UDP `PRE_LOGIN` did not match our camera — the firmware's
PRE_LOGIN flow works). Notes that matter for a reliable session:

- After discovery, a short stabilization pause helps the camera leave discovery mode
  before login.
- Two "magic" ACK-shaped frames (`Magic1`, `Magic2`) are sent around login with fixed
  bytes; their exact purpose is not fully understood but the sequence works.
- **Settle before commanding:** send the first application command only after a heartbeat
  has been acknowledged. Commanding immediately (while the camera still retransmits the
  login response) makes the camera stop responding.
- **Device info before media list**, and `768` only with its paging parameters (§5.4).
- The camera retransmits unacknowledged DATA (including each response fragment) until it
  is acknowledged, so every received DATA/fragment must be ACKed promptly.

---

## 8. Sources

- Own implementation: `components/camera_link/` (framing, reliable-UDP session),
  `components/session_controller/` (lifecycle).
- Own TC100 captures (framing, discovery, ALIVE, heartbeat).
- Public `pi_trailcam` project (command IDs, login choreography, timing observations).
