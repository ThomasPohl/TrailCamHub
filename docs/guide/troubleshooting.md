# Troubleshooting by symptom

Start with the hub's **status**: type `status` on the serial console or use
the *Check the hub* step of the [web installer](../installer.md). It shows the
firmware, the mode that applies, the SD card, the battery and the last cycle.
The log is in the installer's console and in `/sdcard/logs/` on the SD card.

## Flashing and setting up

| Symptom | Likely cause and what to do |
| --- | --- |
| The installer does not list my board's port | Use Chrome or Edge on a computer (Web Serial is not in other browsers or on phones), and a USB **data** cable; try another cable and port. |
| The board is flashed but the console stays silent | Press reset. On the main board the console is the USB-C port with the **USB DIP switch ON** (the modem is then off); put it to OFF again for modem use. |
| `config set …` says `this board has no modem` (or similar) | The setting needs a capability this board lacks; see [`modes.md`](../modes.md). |
| A hub with older firmware refuses a new release | The project was renamed once (#238): flash it once over USB with the installer. |

## The hub does not read the camera

| Symptom | Likely cause and what to do |
| --- | --- |
| Nothing happens after power-up | A battery hub is **idle** until the schedule or `scan`; for the first 10 minutes after power-on it stays awake. Send `scan`. |
| `no camera found` | Camera too far (BLE range about 10 m), camera batteries low, or the camera not awake: wake it with its own button once. Move the hub closer to test. |
| Found, but the Wi-Fi join fails | The camera's access point is weak behind walls. Try again closer; check the camera's batteries (below ~20 % it also reads less often). |
| Login or media list fails after the join | Camera password changed in the TrailCam Go app; set it on the hub (`modes.md`, camera login). |
| Reads work, thumbnails are missing | A thumbnail transfer that stalls is retried; check the log for stalls and the SD card's health. |

## The pictures do not arrive

| Symptom | Likely cause and what to do |
| --- | --- |
| Modem hub: `the SIM card refused this PIN` | Set the right 4-digit PIN again (`config set sim_pin …`). The hub never retries a refused PIN so the SIM is not locked ([`modem.md`](../modem.md#sim-pin)). |
| Modem hub: no registration or no data | Wrong APN, plan without LTE Cat-1, no coverage. Test the SIM in a phone; try `config set apn …`. A weak 18650 cell can reset the board during modem peaks. |
| Gallery shows nothing new | A sleeping hub reports at its next check-in only; look at *last seen* in the administrator's view. |
| WebDAV: nothing arrives | Check the URL, user and app password with a browser or a WebDAV client; `https://` needs a certificate from a public authority, otherwise use `http://` in a trusted network. |
| A video was refused | Too large for the modem uplink, no longer on the camera, or the download failed three times; the gallery says which. |
| Web UI: downloads answer `503` | A camera session is running and uses the SD card; wait for it to finish. |
| ESP32-WROOM-32D check-ins fail | The board has very little memory for TLS (#244); the administrator's view warns when the free heap gets low. |

## Still stuck

Open an issue with the *Problem report* template and attach the `status`
output and the last log lines (remove passwords and keys first). The
[camera compatibility page](compatibility.md) says how to report a new camera.

## Privacy in short

The hub stores everything on **your** SD card. With the own backend the
thumbnails and requested videos are stored in **your** AWS account (private
bucket, links that expire); there is no server run by the project. WebDAV and
the web UI keep the pictures in your home network. The hub's firmware updates
are downloaded from the project's GitHub releases. Keys, passwords and SIM
PINs are stored on the hub only and are never shown again. Think of the
people on your pictures: a camera that sees a public path or a neighbour's
garden needs the usual care and local rules.
