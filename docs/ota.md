# Firmware updates from GitHub

Hubs in the field update themselves to the newest **release**. A release is a git tag `vX.Y.Z`; development builds (a commit hash as version) are never replaced automatically.

## Publishing a release

```bash
git tag v1.0.0
git push origin v1.0.0
```

The firmware workflow builds every board with the tag as version (`TRAILCAMHUB_VERSION` → `PROJECT_VER` → `esp_app_desc`), then the `release` job creates a GitHub release with:

- `trailcamhub-<board>-<variant>.app.bin`: the application image (for OTA) of each board (`esp32s3`, `tdongle-s3`, `tdongle-c5`, `esp32p4`) and variant (`dev`, `release`),
- the deployment packages (`trailcamhub-<board>.tar.gz`, as built for the installer),
- `ota.json`, the manifest the hubs read:

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

## Hub

The hub reads the manifest itself; no server is involved (#150). At the end of a successful sync (after thumbnails, videos and the acknowledgement) `ota::check_for_update()` runs:

1. **May it update at all?** (`ota::ready()`, before anything is downloaded)

   | Case                                                                            | Result   |
   | ------------------------------------------------------------------------------- | -------- |
   | `CONFIG_OTA_AUTO_UPDATE` off                                                    | no check |
   | running version is not `vX.Y.Z` (development build)                             | no check |
   | hub battery in the critical or low level (`docs/power-and-schedule.md`)         | no check |
   | hub battery below `CONFIG_OTA_MIN_BATTERY_PERCENT` (40 %) and not charging      | no check |
   | over the modem with a signal below `CONFIG_OTA_MODEM_MIN_SIGNAL_DBM` (−105 dBm) | no check |

   An unknown battery (no gauge) or an unknown signal does not block. The battery values are those read between cycles, not under the modem's load.

2. **Manifest due?** At most every `CONFIG_OTA_CHECK_INTERVAL_H` (24 h; the time of the last read is kept in NVS, also when the read failed). The console command `update` syncs and reads it right away. One read costs about 10–20 KB.

3. **Read** `CONFIG_OTA_MANIFEST_URL` (GitHub's "latest release", following its redirect) and take the image `<board>-<variant>`.

4. **Decide** (`ota::decide()`, host-tested):

   | Case                                                    | Result                                                                                |
   | ------------------------------------------------------- | ------------------------------------------------------------------------------------- |
   | offered version not newer                               | not installed                                                                         |
   | offered version was rolled back on this hub before      | not installed (a later release is)                                                    |
   | this version failed `CONFIG_OTA_MAX_ATTEMPTS` (3) times | waits `CONFIG_OTA_RETRY_AFTER_H` (24 h) after the last failure, then one more attempt |
   | otherwise                                               | installed                                                                             |

   The failures are counted per version in NVS; a newer release starts afresh, a successful install clears them.

5. **Install:** `ota::install()` downloads the image over HTTPS straight into the other OTA slot, block by block. Before the first block is written it checks the image's own description (project name and version must match the manifest); at the end the size and the SHA-256 of the manifest, then `esp_ota_end()` checks the image itself. A download that breaks off is continued with an HTTP range request from the last byte written (up to three times, while the hub is running); GitHub's asset storage supports ranges.

6. **Restart** once the cycle is over: the uplink ends as usual (over the modem: data call ended, radio off), the scheduler waits until the modem is down (at most 30 s) and restarts the hub (the SD log is written first).

The console's `status` shows the running firmware and the result of the last check (`TCH INFO firmware v1.2.0 (esp32s3-release), release v1.3.0 not installed: the hub battery is too low`).

A hub only checks while it syncs, i.e. with a backend configured; other destinations (#150) will call the same check.

## Backend

The backend no longer offers firmware (#150). It still receives `hubFirmware`, `hubBoard` and `hubVariant` with every check-in.

## Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. A new firmware boots as "pending verification" and confirms itself after its **first successful check-in** (`ota::confirm_running()`). If it restarts before that (crash, watchdog, or deep sleep after a failed check-in), the bootloader boots the old firmware again, which then does not install that version again.

The rollback needs a bootloader built with this option. An update over the air does not replace the bootloader, so a hub flashed before this option existed has to be flashed once with the installer.

## Board and variant

`CONFIG_HUB_BOARD_ID` names the board of the images (set from the board options), `CONFIG_HUB_BUILD_VARIANT` the variant (`dev`; `sdkconfig.defaults.release` sets `release`). An update keeps both, so a release hub stays on release images.

## Not covered yet

- Reporting updates and failed checks to the backend and the app (#149, backend-only for now).
- Showing the hub firmware and the last update in the app (#144).
