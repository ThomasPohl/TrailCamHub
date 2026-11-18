# Notes for Claude Code sessions

TrailCamHub: ESP32 firmware that wakes TC100 trail cameras over BLE, reads
them over their Wi-Fi (PPPP/ARTEMIS), stores media on SD and syncs to an AWS
backend with a Telegram Mini App. Protocol and design notes live in `docs/`.

## Working economically

- Read the issue first, then only the `docs/` page for the task (index below).
  Do not explore the whole repository or read all of `docs/`.
- Name files and acceptance criteria up front; ask before starting a large or
  ambiguous task.
- Do not paste whole logs; quote the relevant lines.

## Where things are

- `components/`: firmware components. `camera_link` (PPPP/ARTEMIS, media),
  `session_controller` (cycle state machine), `schedule*` (plan, deep sleep),
  `uplink`, `modem_uplink`, `ota_update`, `hub_config`, `hub_console`,
  `web_ui`, `dongle_ui`. Pure logic goes in modules without ESP-IDF
  dependencies.
- `main/`: `app_main` and the start-up order.
- `boards.json` + `sdkconfig.defaults*`: one entry and one defaults file per
  board (including `wroom32d`). Other options are in each component's `Kconfig`.
- `tools/`: `camera_mock/` (camera mock for an ESP32-WROOM), `dongle_console.py`.
- `web/` installer, `app/` Mini App, `backend/` AWS SAM (optional, not being
  extended), `test/host/` host tests.

## Docs index (`docs/`)

- Components, one cycle: `architecture.md`
- Build, flash, Kconfig, profiles: `development.md`
- Modes, settings, where a setting lives: `modes.md`
- Boards, wiring: `hardware.md`, `wiring-wroom32d.md`
- Wire format, encryption: `udp-protocol.md`, `protocol-analysis.md`
- Media, SD layout, download, delete: `media-identity.md`, `sd-layout.md`,
  `video-download.md`, `camera-media-delete.md`, `camera-actions.md`
- Clock: `clock.md`
- Power, schedule, modem, OTA: `power-and-schedule.md`, `modem.md`, `ota.md`
- Installer: `installer.md`
- Decisions, roadmap: `decisions.md`, `roadmap.md`

## Git setup after cloning

Session clones are shallow and single-branch, so a pushed branch gets no
`origin/<branch>` ref and the stop hook reports "unpushed commits". Right
after cloning run `git remote set-branches origin '*'` (the clone stays
shallow). Then:

- Create branches from an up-to-date `main` (`git fetch origin main`).
- Push with `git push -u origin <branch>` and check the upstream
  (`git status -sb`).
- Open PRs with `gh api repos/ThomasPohl/TrailCamHub/pulls`. GraphQL (`gh pr`,
  `gh issue list`) is not available in these sessions; use the REST API.

## History is shared: never rewrite it

- No force pushes to `main` or any shared branch; no rebase or amend of pushed
  commits. Fix mistakes with new commits.
- `installer-builds` is written only by the `deploy-installer` job (#207). Do
  not push to it by hand.
- No modified timestamps (`GIT_AUTHOR_DATE`, `GIT_COMMITTER_DATE`, `--date`).
- Take over fork PRs with a merge commit; resolve conflicts between fork PRs in
  a separate `merge/...` PR with a merge commit.

## Changes and pull requests

- Every change is its own PR; reference the issue (`Fixes #NN`).
- Repository content (code, comments, docs, commits, PRs, issues) is in
  English. Conversation with the maintainer is in German.
- New findings from reviews go into a GitHub issue, not only into notes.

## Checks before pushing

Firmware (C/C++):

- `-Wall -Wextra -Werror`.
- Formatting: CI runs clang-format 23.1.3 (`pip install clang-format==23.1.3`).
  Local clang-format 18 is usually identical; on a difference CI wins.
- Host tests (doctest, `test/host/`):

  ```bash
  cmake -S test/host -B build-host -DCMAKE_BUILD_TYPE=Debug
  cmake --build build-host --parallel
  ctest --test-dir build-host --output-on-failure
  ```

- The ESP-IDF build runs only in CI, and CI only compiles. Runtime errors (for
  example missing initialisation in `app_main`) only show on hardware, so
  review start-up and runtime paths explicitly.

Mini App (`app/`): `npm ci`, then `npm run lint`, `npm run typecheck`,
`npm test`, `npm run format:check`. Backend (`backend/`) has its own
`package.json` and tests. In cloud sessions `npm ci` crashes ("Exit handler
never called"); these tests then run only in CI.

## Pitfalls

- ESP-IDF resolves `REQUIRES` before Kconfig: branch on `IDF_TARGET`, not on
  `CONFIG_*`. Registry components are named `espressif__<name>` and must be
  listed explicitly in `(PRIV_)REQUIRES`.
- `app_main` must call `esp_netif_init()` and create the default event loop
  (missing in #32, fixed in #41).
- Do not log raw UDP packets at INFO; it slows the transfer. Use DEBUG.
- Bulk channel: the 16-bit sequence runs globally; a record starts at its
  ARTEMIS header frame (`udp-protocol.md`).

## Time handling

Times that come from the camera (`media_time` / `mediaTime`, `timeUTC`) are
its wall clock read as if it were UTC, not real UTC instants. Convert with
`camera_wall_clock_to_utc()` (firmware) and format with `formatCameraTime()`
(app). See `docs/clock.md`. Never write the camera time back to the camera.
