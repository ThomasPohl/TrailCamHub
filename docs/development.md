# Development

How to build, test and change the firmware. The components and the session
flow are described in [architecture.md](architecture.md).

## Build and flash

The firmware is an ESP-IDF 5.5 project (CI and the dev container use
`espressif/idf:v5.5.6`). For the main board:

```sh
idf.py set-target esp32s3
idf.py menuconfig        # optional, see below
idf.py build flash monitor
```

Other boards add their defaults file from [`boards.json`](../boards.json) in
a build directory of their own, for example the T-Dongle-C5:

```sh
idf.py -B build-tdongle-c5 -D IDF_TARGET=esp32c5 \
    -D SDKCONFIG=build-tdongle-c5/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdongle-c5" \
    build
```

The board and the preset are in `main/Kconfig.projbuild` (menu
**TrailCamHub**). Every other option is in the `Kconfig` file of the
component that uses it, in `menuconfig` under **Component config →
TrailCamHub: …** (for example `components/session_controller/Kconfig`,
**TrailCamHub: session**). An option used by several components is with the
one that owns the function. [modes.md](modes.md#where-a-setting-lives) lists
which settings are build options and which can be changed on the hub.
Useful ones while investigating a camera:

- **Wi-Fi enable payload (hex)** — `BT_KEY_ON` by default; spaces, colons and
  hyphens may separate the bytes; empty = only inspect GATT profiles. The hub
  writes with response if the characteristic supports it.
- **Log camera UDP packet contents as hex** — every camera packet in 16-byte
  lines below its summary.
- **Memory log interval** — free and minimum heap (internal and PSRAM) every
  30 s; 0 logs only at fixed stages.

At boot the firmware logs the reset reason, PSRAM, whether a core dump is
stored (read it with `idf.py coredump-info`) and mounts the SD card at
`/sdcard` without ever formatting it; a missing card is not fatal.

## Build profiles

- **Development** (default): only `sdkconfig.defaults` (+ the board file).
  Debug-friendly optimisation and INFO logs including the camera packets.
- **Release**: adds `sdkconfig.defaults.release` — optimised for size,
  assertion messages silenced, INFO/DEBUG and Bluetooth stack logs removed at
  compile time. Use it to judge size and RAM headroom.

```sh
idf.py -B build-release -D IDF_TARGET=esp32s3 \
    -D SDKCONFIG=build-release/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" \
    build size
```

The VS Code task **ESP-IDF: Build release firmware and show size** runs the
same. Settings for all profiles belong in `sdkconfig.defaults`; size settings
that hinder debugging in `sdkconfig.defaults.release`.

## Host tests

Parsers, packet builders, state machines and schedules have no ESP-IDF
dependency and are tested on the development machine with
[doctest](https://github.com/doctest/doctest), including packets captured
from a TC100. No board and no ESP-IDF are needed:

```sh
cmake -S test/host -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
```

CI runs them a second time with AddressSanitizer and
UndefinedBehaviorSanitizer (`-DHOST_TESTS_SANITIZE=ON` in a build directory
of its own); a finding fails the run.

Files covered by host tests must not include ESP-IDF headers. New parsing or
protocol logic belongs in such files, with a test in `test/host`.

## Camera mock

For bench tests without the real camera, an ESP32-WROOM can play the TC100:
[`tools/camera_mock/`](../tools/camera_mock/README.md) is a separate ESP-IDF
project that advertises like the camera, opens its access point and answers
discovery, login, device info and the media list with responses replayed
from real captures. A hub runs a full `camlist` cycle against it.

## Dev container and Codespaces

The [Dev Container configuration](../.devcontainer/devcontainer.json) uses
the same `espressif/idf:v5.5.6` image as CI
([`Dockerfile`](../.devcontainer/Dockerfile)), mounts the repository at
`/project` and adds the tools of the other CI checks: clang-format 23.1.3,
Node 22 for `app/` and `backend/`, and the GitHub CLI. Every shell has
ESP-IDF set up, ccache is on, and `/root/.cache` is a volume that survives
rebuilds. To flash and monitor boards on USB from the container, use
[`.devcontainer/hardware/`](../.devcontainer/hardware/README.md).

Keep the `/project` mount point: CMake records absolute paths in
`build/CMakeCache.txt`. To apply a changed configuration to a Codespace, run
**Codespaces: Rebuild Container** (or **Dev Containers: Rebuild Container**);
this keeps `build/`. Check the environment with:

```sh
printf 'IDF_PATH=%s\n' "$IDF_PATH"   # /opt/esp/idf
idf.py --version                    # 5.5.6
```

Build with the default VS Code task **ESP-IDF: Build firmware**
(`Ctrl+Shift+B`) or `idf.py build`; on a fresh checkout run
`idf.py set-target esp32s3` once. `build/` and the generated `sdkconfig` are
ignored by Git; keep `build/` for incremental builds instead of running
`idf.py fullclean` routinely.

## Continuous integration

| Workflow | What it does |
|---|---|
| **Firmware Package** | builds every board (development and release), the camera mock, runs the host tests (also with ASan/UBSan) and the clang-format check, reports size per component (on PRs compared with the target branch, warning below 10 % free), publishes the installer from `main` and releases from tags |
| **App** | lints, tests and builds the Telegram Mini App |
| **Backend** | tests and deploys the optional AWS backend |
| **Badges** | updates the README badges |

Before pushing: format with clang-format 23
(`git ls-files '*.c' '*.cpp' '*.h' '*.hpp' | xargs clang-format -i`) and run
the host tests. The ESP-IDF build only compiles; start-up and runtime paths
need a review or a test on hardware.
