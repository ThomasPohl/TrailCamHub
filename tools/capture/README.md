# Capturing the camera app

Scripts to read what the TrailCam Go app sends to the camera, used for the
settings in [`docs/camera-settings.md`](../../docs/camera-settings.md) (#197).
Python standard library only; AES runs through the `cryptography` package if
it is installed, otherwise through the `openssl` command.

## Capture (iPhone and Mac)

The app talks to the camera's own Wi-Fi (`TrailCam_XXXX`, camera at
192.168.43.1), so a router never sees this traffic. Capture on the phone
instead, over USB:

1. Install Xcode once (it brings `rvictl`), connect the iPhone and trust the Mac.
2. `rvictl -s <UDID>` creates the interface `rvi0`. If it fails with "Starting
   device failed", run
   `sudo launchctl load -w /Library/Apple/System/Library/LaunchDaemons/com.apple.rpmuxd.plist`.
3. Start the capture **before** opening the app:
   `sudo tcpdump -i rvi0 -n -w capture.pcap` (no filter, the camera search is a
   broadcast).
4. In the app change one setting at a time, wait about 10 s, and note the time
   and what the app shows.
5. Stop with Ctrl+C and remove the interface with `rvictl -x <UDID>`.

On Android, PCAPdroid (VPN based, no root) can work, but apps that bind their
socket to a Wi-Fi without internet bypass the VPN.

## Decode

```bash
python3 artemis_decode.py capture.pcap --json capture.jsonl   # heartbeats hidden
python3 artemis_decode.py capture.pcap --cmd 513               # only these cmdIds
python3 artemis_diff.py run1.jsonl run2.jsonl                  # what changed in 512/513
```

`artemis_decode.py` reads pcap and pcapng with any link type (including
`rvi0`), reassembles fragmented messages per direction and DRW channel, drops
retransmissions and prints each message with its local time, direction, AppSeq
and decrypted JSON. Packets that are not ARTEMIS JSON (the live picture between
258 and 259) are only counted.

`artemis_diff.py` prints every request in full and, per response, only the
fields that changed since the previous response with the same cmdId.

## Secrets

Captures and decoded output contain the camera's screen lock code
(`secureCode0` … `secureCode3`). Do not commit them or attach them to issues.
