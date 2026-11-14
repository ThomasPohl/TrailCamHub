# Wiring the ESP32-WROOM-32D board

This page shows how to connect a microSD reader to an ESP32 dev board with a
WROOM-32D module, image `wroom32d` (#77). Board notes and limits (no PSRAM,
thumbnails only, mains power) are in [modes.md](modes.md#esp32-wroom-32d-with-an-spi-microsd-reader).

## What you need

- An **ESP32-DevKitC V4** or a clone with **38 pins** (two rows of 19) and an
  ESP32-WROOM-32D or -32E module. Boards with 30 pins have a different pin
  order; the GPIO numbers stay the same, so follow the labels printed on your
  board.
- A **microSD reader breakout** with SPI pins (VCC, GND, MISO, MOSI, SCK, CS).
- Six female-to-female jumper wires, as short as possible (under 10 cm).
- A microSD card, FAT32, up to 32 GB (SDHC).

## Which supply voltage?

Check the reader before you wire it. There are two common kinds:

| Reader | How to recognise it | VCC goes to |
| ------ | ------------------- | ----------- |
| 3.3 V reader | no regulator, often marked "3.3V" | **3V3** (J2 pin 1) |
| 5 V reader | blue board with an AMS1117 regulator and a 74LVC125A chip, often marked "4.5–5.5 V" | **5V** (J2 pin 19) |

A 3.3 V reader on the 5V pin can destroy the card. A 5 V reader on 3V3 gets
too little voltage after its regulator and fails to mount the card.

## Wiring (3.3 V reader)

![ESP32-DevKitC V4 with all 38 pins, wired to a 3.3 V microSD reader](images/wroom32d/sd-wiring-3v3.svg)

Hold the board with the antenna up and the components facing you. J2 is the
left row, J3 the right row; pin 1 of each row is next to the antenna.

| Reader | Board pin | Row / position | Firmware setting |
| ------ | --------- | -------------- | ---------------- |
| VCC    | 3V3       | J2 / 1         | |
| GND    | GND       | J3 / 1         | |
| MOSI   | IO23      | J3 / 2         | `CONFIG_SD_CARD_CMD_GPIO` |
| MISO   | IO19      | J3 / 8         | `CONFIG_SD_CARD_D0_GPIO` |
| SCK    | IO18      | J3 / 9         | `CONFIG_SD_CARD_CLK_GPIO` |
| CS     | IO5       | J3 / 10        | `CONFIG_SD_CARD_CS_GPIO` |

For a **5 V reader** the only change is VCC: connect it to **5V** (J2 / 19)
instead of 3V3. The signal lines stay the same.

Any GND pin works (J2 / 14, J3 / 1, J3 / 7); J3 / 1 keeps all six wires
close together.

## Pins to leave alone

- **D0–D3, CMD, CLK** (bottom of both rows, near USB): connected to the
  module's flash. Using them crashes the board.
- **IO0, IO2, IO12, IO15**: strapping pins that decide how the chip boots.
  IO5 is one too, but it must be high at boot, which the reader's CS pull-up
  ensures.
- **IO34, IO35, VP, VN**: inputs only.
- **TX, RX**: the console over USB.

## Check it

1. Insert the card, then plug in USB. Use only one supply (USB, or 5V, or
   3V3), never two at once.
2. Flash the `wroom32d` image with the [installer](installer.md).
3. The boot log first prints `Mounting SD card (SPI, SCK=18 MOSI=23 MISO=19
   CS=5)` and then `SD card mounted at /sdcard: … MB total, … MB free`.
   If it reports `Could not initialize SD card` instead:
   - check VCC against the table above,
   - swap MISO and MOSI if they are mixed up (the reader may label them
     DO/DI: DO = MISO, DI = MOSI),
   - shorten the wires.

   `SD card found, but its file system could not be mounted` means the
   wiring works: format the card as FAT32 on a computer.

Pin positions follow the header tables in Espressif's
[ESP32-DevKitC V4 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).
