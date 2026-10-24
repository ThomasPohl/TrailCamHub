# TrailCamHub ESP32-S3 Deployment Package

This archive contains the firmware images for the ESP32-S3 BLE proof of concept.

## Browser installer

For the latest `main` build, use the hosted [TrailCamHub browser installer](https://thomaspohl.github.io/TrailCamHub/). It requires a desktop Chromium-based browser with Web Serial support and an HTTPS connection. Connect the board with a USB data cable, choose its serial port, and confirm the browser's prompt. After installation completes, connect to the serial console on the same page to view ESP-IDF output at 115200 baud.

The installer page is deployed by the **Firmware Package** GitHub Actions workflow after pushes to `main`. Pull requests build a package but do not update the public installer. The page's merged image and manifest come from the same workflow build. The default CI firmware sends the `BT_KEY_ON` Wi-Fi-enable payload after BLE discovery.

## Flash the merged image

Connect the board over USB, identify its serial port, and run:

```sh
esptool.py --chip esp32s3 --port PORT --baud 460800 write_flash 0x0 trailcamhub-esp32s3.bin
```

Replace `PORT` with the board's serial port. On Linux this is commonly `/dev/ttyACM0` or `/dev/ttyUSB0`; on macOS it is commonly `/dev/cu.usbmodem...`; on Windows it is a `COM` port.

The merged image includes the bootloader, partition table, and application. The separate images and `flasher_args.json` are included for workflows that flash components individually. Verify `SHA256SUMS` before flashing.

## PoC behavior

The default CI build scans nearby BLE devices and connects to connectable advertisements in turn to inspect their GATT profiles; it does not filter by advertised name and skips non-connectable advertisements. It logs characteristics from discovered services and sends ASCII `BT_KEY_ON` (`42 54 5F 4B 45 59 5F 4F 4E` in hex) only when service `0x8801` has writable characteristic `0x0002` and notification/indication characteristic `0x0003`. The payload can be changed or disabled in `menuconfig`. Use this firmware where connecting to nearby BLE devices for inspection is authorized.

The firmware logs status over its serial console. Use an authorized camera and review its status before leaving the device unattended.