# Profiles, wake-up schedule and power

The firmware serves two use cases from one code base. The **board** is the
hardware axis (pins, PSRAM, console); the **profile** (`CONFIG_HUB_PROFILE_*`,
menu *TrailCamHub PoC → Use case profile*) is the second axis and selects the
defaults that belong to the use case. Every option a profile sets can still be
changed on its own.

| Setting                                     | Autonomous             | On site                |
| ------------------------------------------- | ---------------------- | ---------------------- |
| Default for                                 | ESP32-S3-A7670E-4G     | T-Dongle-C5/-S3, P4    |
| Camera read plan (`SCHEDULE_CAMERA_PLAN`)   | `*=120` (every 2 h)    | empty (no reads)       |
| Check-in interval (`SCHEDULE_CHECKIN_...`)  | 30 min                 | 0 (none)               |
| Deep sleep between cycles (`POWER_DEEP_...`)| on                     | off                    |
| Modem sleep on the home Wi-Fi               | on                     | off                    |
| SD log write interval                       | 10 s                   | 1 s                    |
| Web UI / access point                       | off                    | per board              |
| Video download                              | off (on request, #55)  | per board              |

## Two schedules

Reading a camera and checking in with the backend cost different things, so
they have separate schedules:

- A **camera read** (scan) wakes the camera over BLE, joins its Wi-Fi and runs
  the session. It costs the camera's battery (its access point is on for a
  minute or two) and gives fresh thumbnails. If the uplink is configured, the
  scan ends with a backend sync, which also restarts the check-in interval.
- A **check-in** (upload) only syncs with the backend: it uploads what is left
  and picks up commands from the app, such as a camera read on request. Its
  interval bounds how long such a command waits.

### Camera read plan

Time windows in local time (`CONFIG_TIME_ZONE`), each with an interval in
minutes, and `*` for the rest of the day:

```
05:00-09:00=30,17:00-22:00=30,*=120
```

- A read is due as soon as the time since the last one has reached the interval
  that applies at that moment. The first read in a window therefore comes at its
  start when the last read is older than the window's interval.
- The first window that contains a time applies; a window may span midnight
  (`22:00-06:00=0`). An interval of 0 means no reads; `off` or an empty plan
  means no automatic reads at all.
- Without a valid clock the windows are ignored and the `*` interval applies
  (if it is 0 or missing, the longest window interval).

### Where the settings come from

In this order of precedence:

1. **Backend** (`config` in the check-in response): set in the app per hub.
   Removed in the app, the hub falls back to the next source.
2. **Hub** (NVS, `hub_config`): `config set camera_plan …` and
   `config set checkin_min …` on the serial console or in the web installer's
   *Device setup*.
3. **Build** (Kconfig, the profile's default).

The console's `status` and every check-in report the plan and interval in
effect, where each comes from, and the next planned read and check-in.

### Adaptive behaviour

- **Failed reads** stretch the read interval 2, 4, 8, 16 times after one to
  four failures in a row, up to one day; a successful read resets it. A read
  fails when the camera is not reached after all attempts, when every camera
  session of the cycle ended with an error before it stored anything (each
  such session counts as an attempt), or when the camera has no SD card (no
  further attempts in that cycle; the sync still runs, so that the app shows
  the missing card). A session that stored some media before an
  error counts as a read: the sync runs, and the front ends show it as
  incomplete (`partial`). Failed check-ins stretch the check-in interval in
  the same way.
- **Low camera battery** (`batPercent` of cmdId 512 below
  `SCHEDULE_LOW_BATTERY_PERCENT`, 20 %) stretches the read interval by
  `SCHEDULE_LOW_BATTERY_FACTOR` (3). The level reaches the backend with every
  check-in.
- **No uplink**: camera reads go on to the SD card; the data is synced at the
  next successful check-in.
- **Read on request**: a `readCamera` command from the app makes a read due
  right after the check-in that brought it. The hub acknowledges it at the
  check-in after the read: `done` if that scan read the camera the command is
  for, `failed` if it did not (another camera being read does not count).

Cycles started by hand (console, button, web UI) count like scheduled ones.

## Several cameras per scan

A scan reads every camera that answers, one after the other, and syncs them
all in one check-in (#57):

1. BLE: wake the first camera that answers (name `TC100_…` or service
   `0x8801`), join its Wi-Fi and read it.
2. The camera is then done for this scan: its BLE address is skipped, and the
   hub looks for a further camera for `SESSION_NEXT_CAMERA_TIMEOUT_S` (20 s).
3. This repeats until no further camera answers, `SESSION_MAX_CAMERAS` (4) are
   done, or the camera part has run for `SESSION_CAMERA_BUDGET_S` (15 min; a
   running session is not cut short).
4. Then the sync.

Retries are per camera (`SESSION_MAX_ATTEMPTS`): a camera that answers over
BLE but cannot be read (Wi-Fi, session) is skipped after its attempts, so that
it does not block the others. A camera without an SD card is done at once.

The status lists each camera with its result (`read`, `partial`, `no-card`,
`failed`), BLE name and id; the console prints `TCH EVENT camera <n> <result>
<name> <id>` for each. The scan counts as a read for the schedule when at least
one camera was read; the weakest camera battery of the scan decides the
low-battery stretch.

Every scan spends the next-camera timeout once after its last camera. A hub
with a single camera can set `SESSION_MAX_CAMERAS` to 1 to save that time.

## Deep sleep

With `POWER_DEEP_SLEEP` the hub sleeps when nothing is due for at least
`POWER_MIN_SLEEP_S` (60 s), until the next read or check-in, at most
`POWER_MAX_SLEEP_MIN` (12 h). While it sleeps, the serial console, a web UI and
a display do not respond.

- After a **power-on or reset** the hub stays awake for
  `POWER_AWAKE_AFTER_BOOT_MIN` (10 min) for the web installer and the console,
  and the first scheduled cycle waits `SCHEDULE_START_DELAY_S` (60 s). Press the
  reset button to reach a sleeping hub.
- Every **serial command** keeps it awake for
  `POWER_AWAKE_AFTER_ACTIVITY_MIN` (5 min) more.
- A **wake-up from deep sleep** starts the scheduled cycle at once and goes back
  to sleep when it is done.

### What survives deep sleep

The schedule state lives in RTC memory, which survives deep sleep but not a
power-on or reset: the last read and check-in, the failure counters, the camera
battery and the pending `readCamera` commands. The times are kept on a
monotonic clock that runs on across deep sleep and does not jump when the wall
clock is set (SNTP, backend), so intervals stay right while the clock is
corrected. The wall clock itself keeps running in deep sleep, and so does the
record of where its time came from (`time_sync`). The SD log continues the
newest file after a wake-up instead of starting one per wake-up, and is written
in full before the hub sleeps. Settings in NVS (local and from the backend)
survive everything.

After a power-on or reset the state starts afresh: the first read and check-in
are due after the start delay.

## Hub battery

The Waveshare ESP32-S3-A7670E-4G has a MAX17048 fuel gauge (I2C address
`0x36`, SDA GPIO 15, SCL GPIO 16) on its 18650 cell. With
`CONFIG_HUB_BATTERY_MAX17048` (default on that board, off on the others) the
hub reads it

- for each check-in (`hubBatPercent`; left out when unknown),
- for the console's `status` (`TCH INFO battery 3987 mV, 82% (MAX17048)`),
- at boot and with every periodic memory log.

The gauge is powered by the cell: without a cell it does not answer and the
level is unknown. The reported percentage is the gauge's own state of charge
(ModelGauge, less affected by the modem's load peaks than the voltage). With
`CONFIG_HUB_BATTERY_PERCENT_FROM_VOLTAGE` it is computed from the cell voltage
instead, along a typical Li-ion curve between `CONFIG_HUB_BATTERY_EMPTY_MV`
(3300) and `CONFIG_HUB_BATTERY_FULL_MV` (4200). The schedule does not react to
the hub battery yet.

## Not yet covered

- The A7670E modem uplink (#54) is not used; the check-in still joins the home
  Wi-Fi. Its power policy (attach only for check-ins) belongs there.
- Per-camera schedules: the schedule is one shared cycle for all cameras of a
  hub (a scan reads every camera that answers, see "Several cameras per scan"
  below); a read request for any camera triggers a scan of all of them.
- Notifications about a low camera battery (#58).
