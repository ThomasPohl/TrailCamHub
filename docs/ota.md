# Firmware updates over the check-in

Hubs in the field update themselves to the newest **release**. A release is a git tag `vX.Y.Z`; development builds (a commit hash as version) are never replaced automatically.

## Publishing a release

```bash
git tag v1.0.0
git push origin v1.0.0
```

The firmware workflow builds every board with the tag as version (`TRAILCAMHUB_VERSION` → `PROJECT_VER` → `esp_app_desc`), then the `release` job creates a GitHub release with:

- `trailcamhub-<board>-<variant>.app.bin`: the application image (for OTA) of each board (`esp32s3`, `tdongle-s3`, `tdongle-c5`, `esp32p4`) and variant (`dev`, `release`),
- the deployment packages (`trailcamhub-<board>.tar.gz`, as built for the installer),
- `ota.json`, the manifest the backend reads:

```json
{
  "version": "v1.0.0",
  "images": {
    "esp32s3-release": {
      "url": "https://github.com/…/trailcamhub-esp32s3-release.app.bin",
      "sha256": "…",
      "size": 1572864
    }
  }
}
```

A tag with a suffix (`v1.1.0-rc1`) becomes a pre-release: GitHub's "latest release", and so the hubs, skip it.

## Backend

`FIRMWARE_MANIFEST_URL` (template parameter `FirmwareManifestUrl`, default `https://github.com/ThomasPohl/TrailCamHub/releases/latest/download/ota.json`, empty = off) is read at most every ten minutes per Lambda container (`src/firmware.ts`). A hub reports `hubFirmware`, `hubBoard` and `hubVariant` with every check-in; when the manifest has an image for `<board>-<variant>` with a higher version than the hub's own release, the check-in response carries it:

```json
"firmware": { "version": "v1.0.1", "url": "https://…", "sha256": "…", "size": 1572864 }
```

## Hub

After the sync (thumbnails, videos, acknowledgement) the hub decides (`ota::decide()`, host-tested):

| Case                                                | Result                             |
| --------------------------------------------------- | ---------------------------------- |
| `CONFIG_OTA_AUTO_UPDATE` off                        | not installed                      |
| running version is not `vX.Y.Z` (development build) | not installed                      |
| offered version not newer                           | not installed                      |
| offered version was rolled back on this hub before  | not installed (a later release is) |
| otherwise                                           | installed                          |

`ota::install()` downloads the image over HTTPS (following GitHub's redirect to its storage host) straight into the other OTA slot, block by block. Before the first block is written it checks the image's own description (project name and version must match the offer); at the end the size and the SHA-256 of the manifest, then `esp_ota_end()` checks the image itself. Only then is the new slot selected and the hub restarts (the SD log is written first). A failed download leaves the running firmware untouched and is tried again at the next check-in.

## Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. A new firmware boots as "pending verification" and confirms itself after its **first successful check-in** (`ota::confirm_running()`). If it restarts before that (crash, watchdog, or deep sleep after a failed check-in), the bootloader boots the old firmware again, which then does not install that version again.

The rollback needs a bootloader built with this option. An update over the air does not replace the bootloader, so a hub flashed before this option existed has to be flashed once with the installer.

## Board and variant

`CONFIG_HUB_BOARD_ID` names the board of the images (set from the board options), `CONFIG_HUB_BUILD_VARIANT` the variant (`dev`; `sdkconfig.defaults.release` sets `release`). An update keeps both, so a release hub stays on release images.

## Not covered yet

- The modem uplink (#54): an update is about 1.5–2 MB. Once the hub can check in over LTE, updates should wait for Wi-Fi or Ethernet, or need an explicit permission.
- The hub battery (#75, #138): no update on a nearly empty battery.
- Showing the hub firmware and the last update in the app.
