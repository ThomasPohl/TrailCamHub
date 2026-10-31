# Video download (playback stream)

Status: implemented in `camera_link` and tried on one camera (TC100, firmware 2.2.5.9): several videos of about 460 frames were received completely in a row, each as an H.264 and an AAC file. Photos are downloaded as files (cmdId 1285, see `media_transfer.hpp`); for a video the camera acknowledges that request but sends no data. The TrailCam Go app gets videos by playing them back. This page summarizes the flow as documented by [`pwnb0t/trailcam-automation`](https://github.com/pwnb0t/trailcam-automation) (`docs/download-video.md`, `docs/video-download-apk-flow.md`, `docs/video-v4-decrypt-notes.md`), whose client reconstructs videos that match the camera's SD-card files frame by frame, and how the firmware implements it.

## Control plane

| Command | Body | Answer |
|---|---|---|
| 769 `START_PLAY_RECORD` | `fileType`, `dirNum`, `mediaNum`, `sessionNo` (random) | `startPbRet`, `videoWidth`, `videoHeight`, `totalFrame`, `totalTime`, `result` |
| 770 `STOP_PLAY_RECORD` | – | – |

The app sends `fileType: 1` for videos, while the media list of our camera reports videos as `fileType: 2`. The camera refuses playback with 1 (`startPbRet: -3`) and accepts 2, so the firmware sends the list's value first and, if the camera refuses (`startPbRet` or `result` not 0), repeats the request once with the app's value 1. Heartbeats continue during the stream.

## Data plane

The frames arrive on bulk subtype `0x02` with a running 16-bit sequence and are acknowledged with the same window ACK as thumbnails. In sequence order, the payloads form a byte stream of consecutive ARTEMIS records (one per video or audio frame, not aligned to the bulk frames). Records of version 4 carry a 108-byte header:

| Offset | Field |
|---|---|
| 8 | `pts_ms` (u32 LE) |
| 16 | data length of a video frame (equals record length − 108) |
| 20 | data length of an audio frame |
| 28, 32 | width, height (0 for audio) |
| 48 | `sessionNo` of the 769 request |

In every 0x1000-byte page of the data, the first 0x60 bytes are AES-128-CBC encrypted with the command key `xs38nul7cqf7m1va` and a zero IV, each page on its own; a page with fewer than 0x60 bytes left stays plain. Decrypted video data is H.264 in Annex-B form (start codes), audio data AAC with ADTS headers. The app drops a marker frame that starts with `00 00 00 01 01`. The camera does not mark the end of the stream; it stops sending, and the app stops it with 770. A preview stream on subtype `0x03` (low-resolution track) is ignored.

## Firmware implementation

- `FrameReorder` puts the bulk frames in order (shared with the photo download), `PlaybackStream` splits the byte stream into records and passes each record's payload on in pieces as it arrives, and `PlaybackDecoder` decodes them on the fly: it reads the 108-byte header, decrypts the encrypted prefix of each page through a 96-byte buffer, passes the rest of the data on unchanged, and drops marker frames and the alignment prefix of a video frame (`playback_record` has the functions for a complete record). The decoder's output is byte for byte that of the complete records, checked by a host test that feeds the same stream in frames of 20 to 1472 bytes and in pieces down to 1 byte (`test/host/test_playback_decoder.cpp`); only when `annexb_start_offset()` depends on the end of a frame (a short start code at 1 to 4 and no long one there) is that frame collected. All are host-tested (`test/host/test_playback.cpp`).
- `camera_protocol.cpp` sends 769 after the photos, writes video frames to `<dir>_<num>.h264` and audio frames to `<dir>_<num>.aac` in `/sdcard/<camera>/media/` (both as `.part` until complete), ends the stream after 3 s without frames, sends 770 and waits until no more frames of the stopped stream arrive (1 s) before the next video. A record with another session number is dropped; if the camera ignores the requested number, its own is followed after 30 records.
- Records are not collected; `CONFIG_CAMERA_DOWNLOAD_VIDEOS` needs PSRAM for the reorder window of 256 frames and the SD writer's 4 MB buffer (on for the T-Dongle-C5).

The raw `.h264` file plays in VLC. One MP4 is made without re-encoding with

```sh
ffmpeg -framerate 25 -i 100_2.h264 -i 100_2.aac -c copy 100_2.mp4
```

(25 frames per second is the rate of the camera tried so far: 459 frames in 18.36 s, and the presentation times step by 40 ms; `totalFrame` and `totalTime` in the log give the rate of another camera). Writing the MP4 on the dongle is a later step, once the streams are verified.

## Observed speed

With the current firmware a 15-second 1080p video (459 frames, about 9.5 MB of H.264 and 124 KB of AAC) takes about 37 s on the T-Dongle-C5, at about 260 KB/s; three videos in a row were received completely, with no failed ACK send. A full sync of 45 such videos therefore takes about half an hour.

The first version took about 340 s for an 18-second video (about 29 KB/s). Two causes were found:

- **Reorder window too small.** The camera keeps sending about 120 frames ahead of the first frame it has no acknowledgement for. With a reorder window of 32 frames, everything beyond it was dropped unacknowledged and sent again later, so only about 32 frames got through per round (after 200 frames: 10,962 frames repeated by the camera, 4,475 beyond the window, but only 5 shortly ahead of the expected one). Thumbnails, at about 20 frames, fit into the window, which is why they were always fast. The window is now 256 frames with PSRAM (about 377 KB per stream); the speed rose to about 218 KB/s.
- **Wi-Fi TX buffers used up.** At that speed, one ACK per frame (a few hundred per second) with the station in modem sleep, where outgoing frames wait until the radio wakes, used up the 16 Wi-Fi TX buffers: thousands of sends failed with `ENOMEM`, including the heartbeat, and the second video stalled. The firmware now switches Wi-Fi power save off while it is connected to the camera (BLE is released then; modem sleep returns on disconnect) and acknowledges playback frames together, one ACK per 4 frames or after 10 ms without a frame; each ACK lists the last 16 sequences.

Measured on 2026-10-31 with the same two videos each time (100_52, 3.7 MB, and 100_51, 9.6 MB, fetched again after removing `catalog.csv`) and the FreeRTOS run-time statistics on, the download was CPU-bound: the single core was idle only 13-17 % of the time, the camera session took about 40 %, the SD writer 26 %. The SD driver waits for the card actively (it polls the busy state with byte-sized SPI transactions), so the card's write rate depends on the CPU time the low-priority writer gets: about 1 MB/s with 8 KB pieces in a benchmark (`sdbench` on the console), but only about 400 KB/s during a download. While the writer's 1 MB buffer was full, the session waited (about 11 s per 9.6 MB video), frames overflowed the receive queues, and the camera sent about 5,800 frames again. Two changes in `sd_writer`:

| | 100_52 | 100_51 | waited for SD (100_51) | sent again (100_51) | SD writer CPU |
|---|---|---|---|---|---|
| before | 214 KB/s | 235 KB/s, 41.2 s | 11.6 s | 5,791 | 26 % |
| data collected per file, written in 16 KB pieces | 261 KB/s | 291 KB/s, 33.2 s | 6.0 s | 4,551 | 15 % |
| and a 4 MB buffer in PSRAM instead of 1 MB | 282 KB/s | 312 KB/s, 31.0 s | 3.2 s | 4,086 | 16 % |
| and pending ACKs sent before waiting for the card, receive timeout set only when it changes | 287 KB/s | 321 KB/s, 30.2 s | 3.8 s | 4,711 | 17 % |
| and the records decoded as they arrive (`PlaybackDecoder`) | 305 KB/s | 297 KB/s, 32.5 s | 4.7 s | 4,685 | 28 % |

Before, every video and audio frame was a write of its own (ending in the middle of a sector), alternating between the two files. Now the H.264 file's data is collected in a 16 KB buffer and the AAC file's in a 4 KB buffer, both in internal RAM, and written when full, sector-aligned; flush() writes the rest before a file is finished. Two downloads of the same video were identical byte for byte. The SD clock stays at 20 MHz: the card does not switch to high speed in SPI mode, and the default speed allows at most 25 MHz, while the next divider step is 26.7 MHz. About a third of the frames the camera sends again arrive while no gap is open. Sending the pending ACK before the session waits for the card did not change that noticeably, so they do not come from ACKs held back by the session; the camera resends them on its own or after a lost ACK. Setting the socket's receive timeout only when it changes (it was set before every frame) lowered the TCP/IP task's CPU share from 7 to 4 %. Fewer sequences per ACK made it slower: with 8 per ACK 272 and 313 KB/s, with 4 per ACK 256 and 293 KB/s, while the camera sent about as many frames again (`CONFIG_CAMERA_BULK_ACK_FRAMES`, default 17 as the app). Decoding the records as they arrive instead of collecting them (up to 3 MB) saves a copy of every byte in PSRAM: the session task's share went from about 45 to 41 %, which the SD writer now uses; the speed stayed within the spread between runs (the 15-second video 30 to 33 s). Both downloads gave the same files as before, byte for byte. The session task, at about 41 % CPU, and the SD writer are the main limits.

A trace of every frame and ACK (`CONFIG_CAMERA_VIDEO_TRACE`, written to `trace_<dir>_<num>.csv` next to the catalog) showed where the frames sent again come from. There is one 769 request per video and no frame on another subtype, so no second stream. About a quarter of all frames came again, mostly one at a time, in the median about 50 ms after our ACK had listed them (only 3-5 % before); 60-70 % while a gap was open. Record starts were not repeated more often than other frames (3 % of the frames, 3 % of the repeats). So the camera resends on a timer of its own or does not get or use part of the ACKs; fewer sequences per ACK did not change it.

The trace also showed that the last frame of a video arrived 3 s before the stream was taken as ended: the firmware waited 3 s without frames, as audio frames could still follow the last video frame. In every traced video the last video frame was the last frame, so once all video frames that the 769 answer announced (`totalFrame`) have arrived, the stream now ends after 0.5 s without frames (still 3 s otherwise; the camera announced 460 frames of 100_51 but sent 459, and 460 of 100_50 but sent 458). After a video that ended normally, the session no longer waits for the SD writer: it queues the end of the video's two files (`sd_writer_end_files()`), sends 770 and requests the next video, which is written to the other of two pairs of files; the files of the last video are closed and the video is marked as stored in the catalog once the writer has written them (or, at the end of the queue and of the session, after waiting for it). Both videos were stored as before, byte for byte. The two videos took 44.1 s from the first request to the end of the second stream instead of 45.1 s: the time to detect the end of 100_52 went from 3.3 to 0.8 s and the pause before the next request from 2.5 to 1.1 s, but 100_51 took 3 s longer, as the writer now finishes 100_52 at the same time and both compete for the single core.

An earlier guess, that the camera paces the stream by the session heartbeat (about two video frames per heartbeat at 1.5 s), did not hold: with the heartbeat at 500 ms (`CONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS`) the speed stayed the same. SD writes took about a third of the transfer time (about 12 s of 37 s); see below for what was changed.

## What the log shows

`ARTEMIS response cmdId 769: {...}` with the camera's answer, `First video frame: WxH, pts …` with the first 16 decrypted bytes (they should start with `00 00 00 01`), progress every 100 frames, and a summary per video: video and audio frames, dropped markers, records of another session and other records, and the file sizes, followed by `Video transfer:` with the counts of frames out of order, frames repeated by the camera, frames beyond the reorder window and failed ACK sends, the time spent in SD writes (in total and the longest), the internal heap (free and minimum) and frames far behind the expected one. `Video transfer:` is also logged with every progress line and when the session ends during a video. The first 40 frames beyond the reorder window, and then every 500th, are logged as `Playback frame seq …, expected … (delta …)` with their first bytes.
