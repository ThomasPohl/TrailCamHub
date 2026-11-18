# Guide: garden hub over 4G with the gallery

A hub next to a trail camera **without Wi-Fi**. It wakes the camera on a
schedule, copies new pictures to its SD card, sends thumbnails over the mobile
network to your own small backend, and the people who look at the pictures use
the **gallery web app** on a laptop or phone. Videos are fetched on request.

Parts: [shopping list, mode A](shopping-list.md#a-garden-hub-over-4g-modem--own-backend--gallery).
Details behind each step: [`modem.md`](../modem.md), [`power-and-schedule.md`](../power-and-schedule.md),
[`backend/README.md`](../../backend/README.md), [`app/README.md`](../../app/README.md).

```
camera ──BLE/Wi-Fi──▶ hub (SD card) ──4G──▶ backend (AWS) ◀── gallery (laptop)
```

## 1. Assemble the hub

1. Insert the microSD card (FAT32) and the nano-SIM. Remove the SIM's PIN in a
   phone first if you can; the hub only handles 4-digit PINs
   ([SIM PIN](../modem.md#sim-pin)).
2. Fit the LTE antenna and the 18650 cell (mind the polarity), connect the
   solar panel.
3. Set the DIP switches: **USB OFF** (modem on the ESP32), **4G ON** (modem
   powered). More in [`modem.md`](../modem.md#board-setup).

## 2. Flash and set up the hub

1. Open the [web installer](https://thomaspohl.github.io/TrailCamHub/) in
   Chrome or Edge on a computer, connect the board with a USB **data** cable.
2. Choose **Waveshare ESP32-S3-A7670E-4G** and install the release build.
3. In the setup assistant choose *battery*, *push to the backend* and
   *4G modem*. Enter the APN of your SIM (see your provider) and the PIN if
   there is one. Leave the backend fields empty for now.
4. Let the assistant check the hub: SD card, battery, firmware.

## 3. Set up the backend (once)

The backend is optional and runs in your own AWS account; the stack stays within
the free allowances at this scale. Follow
[One-time AWS setup](../../backend/README.md#one-time-aws-setup) (bootstrap,
access key, GitHub settings, run the *Backend* workflow). Note the API URL
the job prints.

## 4. Register the hub and the users

On a computer with the AWS CLI, in `backend/`:

```bash
npm run register-hub -- --hub garden-hub --name Garden
npm run add-user -- --name "Opa" --hub garden-hub
```

- `register-hub` prints the **hub key**.
- `add-user` prints a **set-up link** for the gallery. Open it once on the
  user's laptop; the gallery remembers the sign-in. There is no
  self-registration; `--admin` makes an administrator who sees all hubs.

Back in the web installer (*Device setup*) enter the **API URL** and the **hub
key** and save.

## 5. First read

1. Put the hub near the camera, powered, with the camera's batteries in.
2. In the installer press **search cameras**, or send `scan` on the console.
   The hub wakes the camera over Bluetooth, reads its list and thumbnails and
   syncs over 4G. The first read can take several minutes.
3. Open the gallery. Cameras the hub **found** appear under "Kameras
   verwalten"; accept the ones the hub should read and give them a name.
   The hub reads only cameras that were accepted.

From then on the hub follows its schedule (default: more often at dawn and
dusk, see [power and schedule](../power-and-schedule.md)) and sleeps in between.

## 6. Day to day

- **Thumbnails** show up after the hub's next check-in; a sleeping hub is
  never instant.
- **Videos**: select a recording in the gallery and ask for the video. The
  hub fetches it at its next read and uploads it over 4G (limit and data cost:
  [modem uplink](../modem.md)). Videos above the modem limit are refused with
  a message.
- **Deleting** a recording in the gallery removes it from the camera too.
- Battery, signal and faults are in the administrator's view of the gallery.

If something does not work, see [troubleshooting](troubleshooting.md).
