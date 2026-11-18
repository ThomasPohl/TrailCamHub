# Shopping list per mode

What to buy for each way of using TrailCamHub. Prices are **rough orientation
for Europe in late 2026** (shop, country and season change them); check the
current price before you order. Boards and their capabilities are in
[`hardware.md`](../hardware.md) and [`boards.json`](../../boards.json), the
modes in [`modes.md`](../modes.md). Not sure which mode fits? Start with the
[decision tree](README.md).

Every hub needs these, whatever the mode:

| Part | Notes | Rough price |
| --- | --- | --- |
| microSD card | FAT32, up to 32 GB (SDHC); 8–32 GB is plenty for thumbnails, more only if you keep videos | 5–10 € |
| USB **data** cable | A charge-only cable does not work for flashing | 3–8 € |
| The camera | A Wi-Fi trail camera that works with the TrailCam Go app; see the [compatibility list](compatibility.md) | already yours |

## A. Garden hub over 4G (modem + own backend + gallery)

For a camera **without any Wi-Fi nearby**. The hub runs on battery and solar
power and sends pictures over the mobile network. Guide:
[modem-gallery.md](modem-gallery.md).

| Part | Notes | Rough price |
| --- | --- | --- |
| Waveshare ESP32-S3-A7670E-4G | The main board (modem, battery gauge, SD slot) | 40–55 € |
| LTE antenna | The board's own antenna if it comes with one; a better one helps at the edge of coverage | 0–10 € |
| nano-SIM with a data plan | The A7670E is LTE Cat-1; 2G/3G-only plans do not work. Videos use data: a pre-paid or IoT plan with a few GB a month is typical | 3–10 € per month |
| 18650 cell | Flat-top or button-top that fits the holder, protected, e.g. Sanyo NCR18650GA (3450 mAh) | 8–12 € |
| Solar panel 6 V / 5 W | Matches the board's default solar setting; angle it to the south | 12–20 € |
| Weatherproof box and cable gland | The hub and the cell must stay dry; do not seal the solar panel's cable in a way that lets water run in | 8–15 € |
| Backend (AWS account) | Optional own backend for the gallery; stays inside the free allowances at this scale except cents for storage, see [`backend/README.md`](../../backend/README.md) | 0–1 € per month |

Total: about 80–120 € once, plus the SIM's running cost.

## B. Hub at home with a NAS (home Wi-Fi + WebDAV)

For a camera **within range of your own Wi-Fi** and a NAS or cloud that
speaks WebDAV (Nextcloud, Synology, QNAP, many routers). Guide:
[home-webdav.md](home-webdav.md).

| Part | Notes | Rough price |
| --- | --- | --- |
| ESP32-S3 or ESP32-WROOM-32D board | Any [supported board](../hardware.md); the WROOM-32D needs an SPI microSD reader and a few wires, see [wiring](../wiring-wroom32d.md). It is the cheapest option but has little memory (#244) | 6–15 € |
| microSD reader (SPI) | Only for the WROOM-32D; boards with an SD slot need none | 2–4 € |
| USB power supply, 5 V / 1 A or more | Mains power; the hub does not sleep deeply in this mode | 5–10 € |
| Your own NAS or WebDAV server | Not part of the project | already yours |

Total: about 15–30 € on top of the NAS.

## C. Hub with its own web page (pull: dongle or Ethernet board)

For someone who **walks past** with a phone or laptop, or a hub on a wired
network. The hub keeps the pictures on its SD card and serves them from its
own web page. Guide: [web-ui.md](web-ui.md).

| Part | Notes | Rough price |
| --- | --- | --- |
| LilyGO T-Dongle-C5 | USB stick with display, button and its own access point; the best tested dongle | 20–30 € |
| LilyGO T-Dongle-S3 | Similar, no PSRAM, so videos are limited; not tested on hardware | 15–20 € |
| Waveshare ESP32-S3-ETH or ESP32-P4-NANO | For a wired network; Ethernet needs a network socket near the camera | 25–60 € |
| USB power supply or power bank | Mains power; a power bank lasts days to weeks depending on the schedule | 5–25 € |

The dongles carry their own microSD slot or need a small adapter; check the
board's page before you order.

## What the project does not cost

There is no subscription and no server run by the project. The only running
costs are your SIM plan (mode A) and, if you use AWS, a few cents for storage.
