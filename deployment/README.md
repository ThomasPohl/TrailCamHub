# TrailCamHub ESP32-S3 Deployment Package

This archive contains the firmware images for the ESP32-S3 BLE proof of concept.

## Browser installer

For the latest `main` build, use the hosted [TrailCamHub browser installer](https://thomaspohl.github.io/TrailCamHub/). It requires a desktop Chromium-based browser with Web Serial support and an HTTPS connection. Connect the board with a USB data cable, choose its serial port, and confirm the browser's prompt. After installation completes, connect to the serial console on the same page to view ESP-IDF output at 115200 baud.

The installer page is deployed by the **Firmware Package** GitHub Actions workflow after pushes to `main`. Pull requests build a package but do not update the public installer. The page's merged image and manifest come from the same workflow build. The default CI firmware performs BLE discovery only; it does not send a Wi-Fi-enable payload.

## Flash the merged image

Connect the board over USB, identify its serial port, and run:

```sh
esptool.py --chip esp32s3 --port PORT --baud 460800 write_flash 0x0 trailcamhub-esp32s3.bin
```

Replace `PORT` with the board's serial port. On Linux this is commonly `/dev/ttyACM0` or `/dev/ttyUSB0`; on macOS it is commonly `/dev/cu.usbmodem...`; on Windows it is a `COM` port.

The merged image includes the bootloader, partition table, and application. The separate images and `flasher_args.json` are included for workflows that flash components individually. Verify `SHA256SUMS` before flashing.

## PoC behavior

The default CI build has an empty Wi-Fi-enable payload. It scans for the configured BLE camera name and discovers service `0x8801` and characteristic `0x0002`, but it does not write to the camera. A verified payload must be configured and built before the firmware can request that the camera enable Wi-Fi.

The firmware logs status over its serial console. Use an authorized camera and review its status before leaving the device unattended.