# Hardware

TrailCamHub runs on several ESP32 boards; the board fixes what the hub can do
(modem, Ethernet, battery gauge, PSRAM, display), the [mode](modes.md)
decides how it is used. The single source for the list is
[`boards.json`](../boards.json): the web installer, CI and the capability
check in CI read it.

| Board | Target | Capabilities | Preset | Status |
|---|---|---|---|---|
| Waveshare ESP32-S3-A7670E-4G | esp32s3 | 4G modem, battery gauge, PSRAM, deep sleep | battery + push | main board, end-to-end tested |
| LilyGO T-Dongle-C5 | esp32c5 | display, button, web UI, PSRAM, deep sleep | mains + pull | boot and video download tested |
| LilyGO T-Dongle-S3 | esp32s3 | display, button, deep sleep; no PSRAM | mains + pull¹ | not tested on hardware |
| Waveshare ESP32-P4-NANO | esp32p4 | Ethernet (IP101), web UI, PSRAM; Wi-Fi/BLE over a C6 | mains + pull | in test |
| Waveshare ESP32-S3-ETH | esp32s3 | Ethernet (W5500), web UI, PSRAM, deep sleep | mains + pull | not tested on hardware |
| ESP32-WROOM-32D + SPI microSD | esp32 | deep sleep; no PSRAM, 4 MB flash | mains + pull¹ | not tested on hardware |

¹ Without a web UI the direction is push.

Pins, wiring and notes for the smaller boards are in [modes.md](modes.md#boards).
Wiring diagram for the ESP32-WROOM-32D and its microSD reader: [wiring-wroom32d.md](wiring-wroom32d.md).
The 4G modem is described in [modem.md](modem.md).

**What you need besides the board:** a microSD card (FAT32, up to 32 GB
SDHC), a USB data cable, for the main board an 18650 cell, optionally a 6 V
solar panel and a SIM card with a data plan.

## Main board: Waveshare ESP32-S3-A7670E-4G

According to the [Waveshare documentation](https://docs.waveshare.com/ESP32-S3-A7670E-4G):

- ESP32-S3R8 with 512 KB SRAM, 8 MB octal PSRAM and 16 MB flash.
- A7670E LTE Cat-1 modem on the **native USB port of the ESP32-S3** (USB
  host, PPP), not on a UART. The S3 has a single USB PHY, so the console uses
  UART0 through the board's USB-to-UART bridge; the bridge and the modem
  share one USB-C port and DIP switches select which one is connected
  (**USB OFF** = modem on the S3, **4G ON** = modem powered).
- microSD slot on SDMMC in 1-bit mode (CLK GPIO5, CMD GPIO4, D0 GPIO6).
- 18650 holder, solar input and a MAX17048 fuel gauge on I2C (0x36,
  SDA 15, SCL 16).
- DIP switches for camera, USB hub and modem power, GNSS antenna connector,
  24-pin camera connector.

`sdkconfig.defaults` and `partitions.csv` target this board: 16 MB flash,
octal PSRAM, two 4 MB OTA slots, a 64 KB core dump partition and 64 KB NVS.
Board revisions V1 and V2 differ in some GPIOs (camera, I2C); the SD pins can
be changed in `menuconfig`.

Tested with a Sanyo NCR18650GA (3450 mAh) and a Waveshare 6 V / 5 W solar
panel, which matches the board's default solar setting.

### Open questions

- Modem power-control GPIO: the Waveshare FAQ names GPIO33, which collides
  with the S3R8's octal PSRAM.
- Power supply and modem peak current on battery; charging below 0 °C (the
  gauge has no temperature input).
- LTE bands, SIM and antennas for the deployment location.

Camera credentials and SIM PINs must never be committed to the repository.
