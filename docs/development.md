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

Every cycle has a number that goes on across deep sleep. The log lines of a
cycle on the SD card start with it (`#12 …`), the console's `status` shows
the last cycle with its lowest free heap and stack, the check-in sends these
figures (`memory`), and `/sdcard/logs/cycles.jsonl` keeps one line per cycle
([sd-layout.md](sd-layout.md#logscyclesjsonl)).

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

The `session_tests` target (part of the same `ctest` run) runs a whole
camera session of `camera_link` against the camera mock core over loopback UDP
(`127.0.0.1:40611`). `camera_link` itself is ESP-IDF code; `test/host/esp_stubs`
provides stand-ins for the ESP-IDF headers and the SD card is a directory. The
tests compare the result, the files on the card and the normalised log of each
scenario, so a change to the session code that alters behaviour shows up. A
run takes about a minute (about five with sanitizers).

`session_virtual_tests` runs the same scenarios without sockets and without
sleeping (#354). The camera mock is called from the session's own thread
through virtual UDP sockets (`test/host/session/virtual_net.cpp`: the packet
I/O of `camera_udp_transport.cpp` is replaced and `socket()`, `bind()`,
`setsockopt()` and `close()` are redirected by the linker), and the clock only
moves while the session waits, so the cases that depend on time (the camera
closes, a stop request, a silent camera, a lossy link) are compared exactly and
repeat identically. A datagram from the camera takes 1 ms of virtual time. The
clock starts at the same value in every case; the result of a session can
depend on the absolute time. The production code is not changed for this.

`test/host/check_camera_session_compile.sh` additionally compiles the session
sources in four Kconfig variants (photos, all videos, videos on request,
discard and preview) with `-Wall -Wextra -Werror`.

Files covered by host tests must not include ESP-IDF headers. New parsing or
protocol logic belongs in such files, with a test in `test/host`.

### Whole-cycle host test: inventory (#446, phase 1)

Goal of #446: one host test that runs complete wake cycles (scheduler decision,
camera sessions, SD catalog, uploads, check-in, commands) against the real
backend code. These are the ESP-IDF calls the cycle components make today, and
so the seams a host build needs (only where the test needs one):

| Component | Direct ESP-IDF use | Seam needed |
| --- | --- | --- |
| `session_controller` | `xTaskCreate`, `xQueue*`, `xSemaphore*`, `esp_timer_*` | none beyond the FreeRTOS stubs: run the task body on a thread, or step the state machine (`session_fsm`, already pure) from the driver |
| `scheduler` | `esp_deep_sleep_start`, `esp_sleep_*`, `esp_wifi_stop`, `xTaskCreate`, `vTaskDelay` | a small `Sleep` interface: "sleep until T" returns to the driver, which starts the next cycle with the stored state |
| `schedule_state` | `nvs_*` (blob and string), `esp_reset_reason`, `esp_timer_get_time` | an in-memory NVS stub (map of namespace/key) and a settable reset reason |
| `uplink` (`backend_http`, `http_util`) | `esp_http_client_*` (init/open/write/fetch_headers/read), `esp_err_to_name`, `esp_log_level_*` | an `HttpTransport` interface, or an `esp_http_client` stub that forwards to a local port; the stub is the smaller step |
| `uplink` (`data_usage_store`) | `nvs_*` | the same NVS stub |
| `uplink` (`backend`, `destinations`, `backend_commands`) | `esp_app_get_description`, `esp_timer_get_time` | the existing `esp_app_desc.h` and `esp_timer.h` stubs, with a virtual clock |
| `uplink` (`backend_checkin`, `sync_cameras`, `video_request`, `delete_request`) | none | none (already pure or stub-only) |

Not needed in phase 1: BLE wake (a no-op that opens the mock's AP state), RTC
memory (a struct kept by the driver across "deep sleeps"), modem and Ethernet.
The backend side is `backend/src/handler.ts` with `store/memory.ts` (phase 2).
Phase 1 is therefore: NVS stub, `esp_http_client` stub, `Sleep` seam in
`scheduler`, then the scenario "first cycle" with two mock cameras.

First step done: `test/host/cycle/` builds `cycle_tests` (part of `ctest`,
also with sanitizers). `cycle/host_cycle.cpp` provides NVS in memory, a
virtual wall clock (`gettimeofday` and `time` are wrapped at link time), a
boot-relative `esp_timer` and the reset reason; the driver calls
`host_cycle::power_on()`, `restart()` or `deep_sleep()` and then
`schedule::init()` like `app_main`. `cycle/stubs/` comes before `esp_stubs/`
in the include path. The tests cover the hub's state across restarts and deep
sleep (`schedule_state` with `hub_config`).

`cycle/host_http.cpp` stands in for `esp_http_client`: each request goes to a
handler the test sets (`host_http::set_handler()`), in process; without a
handler a request fails like without a network. The uplink's `http_util` and
the data usage counter run on it. Phase 2 sets a handler that forwards to the
local backend.

The scheduler's decision while the controller is idle (request a read or a
check-in, sleep, or wait) is the pure `schedule::scheduler_step()`
(`components/schedule/scheduler_step.cpp`); `scheduler_task` acts on it.
`cycle/test_day.cpp` runs a battery hub over a virtual day with it (boots,
deep sleeps, quiet hours) and checks the list of cycles. Next: the scenario
"first cycle" with two mock cameras and the backend.

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

## Static analysis

clang-tidy checks the files that build without ESP-IDF (the sources of the
host tests) with the checks in `.clang-tidy`; every finding fails CI:

```sh
.github/scripts/static-analysis.sh      # uses or creates build-host
```

Fix a finding, or suppress it with a `NOLINT(<check>)` comment that says why.
CI uses the clang-tidy of Ubuntu 24.04 (18).

## Continuous integration

| Workflow | What it does |
|---|---|
| **Firmware Package** | builds every board (development and release), the camera mock, runs the host tests (also with ASan/UBSan), clang-tidy on the files of the host tests and the clang-format check, reports size per component (on PRs compared with the target branch, warning below 10 % free) and the static RAM of every board (see below), publishes the installer from `main` and releases from tags |
| **App** | lints, tests and builds the gallery web app |
| **Backend** | tests and deploys the optional AWS backend |
| **Badges** | updates the README badges |

Before pushing: format with clang-format 23
(`git ls-files '*.c' '*.cpp' '*.h' '*.hpp' | xargs clang-format -i`) and run
the host tests. The ESP-IDF build only compiles; start-up and runtime paths
need a review or a test on hardware.

The summary of every board build has a **Static RAM** table
(`.github/scripts/ram-report.sh`): used and free bytes of the RAM regions
(DRAM and IRAM on the ESP32; one shared region on the newer chips, called
DIRAM or HP SRAM) for the development and the release image. What is free of
the data RAM is the heap at boot. The job warns below 10 % free IRAM on the
ESP32 (the link fails when it is full, #209) and below 25 % free data RAM.
