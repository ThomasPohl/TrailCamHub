# Mode, wake-up schedule and power

The firmware serves two use cases from one code base. The **board** fixes the
hardware (its capabilities: modem, Ethernet, battery gauge, web UI, display);
the **mode** says how it is used and is chosen at run time, see
[`modes.md`](modes.md). Two of its settings decide the schedule and the power
behaviour:

| Mode                                         | Battery                | Mains                  |
| -------------------------------------------- | ---------------------- | ---------------------- |
| Camera read plan (`SCHEDULE_CAMERA_PLAN`)     | `*=120` (every 2 h)    | `*=120` with push to a configured destination, else empty (no reads) |
| Check-in interval (`SCHEDULE_CHECKIN_...`)   | 30 min with push, 0 with pull | the same        |
| Quiet hours (`SCHEDULE_QUIET`)                | `23:00-06:00`          | none                   |
| Location (`SCHEDULE_LOCATION`)                | empty (fallback)       | empty (fallback)       |
| Deep sleep between cycles (`POWER_DEEP_...`) | on (if built in)       | off                    |
| Modem sleep on the home Wi-Fi                | on                     | off                    |

The build's preset (`CONFIG_HUB_PROFILE_*`, menu *TrailCamHub → Preset
mode*) is the mode of a hub whose `power` and `direction` settings are empty:
battery + push on the ESP32-S3-A7670E-4G, mains + pull on the T-Dongles and
the ESP32-P4-NANO. A few build options still follow the preset rather than the
mode:

| Build option                | Battery + push preset  | Mains + pull preset    |
| --------------------------- | ---------------------- | ---------------------- |
| SD log write interval       | 10 s                   | 1 s                    |
| Web UI / access point       | off                    | per board              |
| Video download              | on request (#55)       | per board              |

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

#### Windows at the sun

A window can start or end at sunrise or sunset (#436), so that it follows the
sun through the year (Berlin: sunrise 04:45 in June, 08:15 in December):

```
sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120
```

- A bound is `HH:MM`, `sunrise` or `sunset`, the last two with `+N` or `-N`
  minutes (at most 720). `..` separates the bounds; `HH:MM-HH:MM` stays valid
  and the forms can be mixed (`05:00..sunrise+30=15`).
- The times come from the hub's position (setting `location`, see below) and
  are computed on the hub for each local day, about ±2 minutes. A window whose
  sun time is unknown (polar day or night) is skipped for that day.
- The backend's own plan editor does not know this form yet; set it on the hub
  (`config set camera_plan …`).

#### Location

Setting `location` is `LAT,LON` in decimal degrees (for example `52.5,13.4`),
stored rounded to 0.1° (about 10 km, which changes the sun times by less than a
minute). Set it with `config set location 52.5,13.4` or in the build
(`CONFIG_SCHEDULE_LOCATION`). Without one the hub uses the middle of Germany
(51.0, 10.0), which is off by up to about 25 minutes at the borders, and says
so in `status` (`location … (fallback, no location set …)`); `status` also
shows today's sunrise and sunset to check the value.

**GNSS at start** (#436, boards with a GNSS-capable modem such as the Waveshare
ESP32-S3-A7670E-4G with its antenna connected): after a power-on or reset, if
no location is set (setting, build) and none was found before, the modem task
switches the receiver on (`AT+CGNSSPWR=1`), polls `AT+CGNSSINFO` for up to
`CONFIG_MODEM_GNSS_FIX_TIMEOUT_S` (180 s; 0 turns it off), switches it off
again and stores the fix rounded to 0.1° (NVS `gnss_loc`, origin `gnss` in
`status`). It runs before the modem goes into flight mode; a sync that starts
meanwhile waits for it, and the first cycles may still use the fallback
position. Without antenna, receiver or sky there is no fix: the hub logs
`GNSS: no fix`, keeps the fallback and tries again at the next power-on or
reset. Order of precedence: set on the hub > found by GNSS > build > fallback.
A stored fix stays; a hub that moved gets its new position with
`config set location …`. Not verified on hardware yet (the answer format is
from the A76xx AT manual).

### Quiet hours

Setting `quiet`, a window in local time (`CONFIG_TIME_ZONE`) that may span
midnight, for example `23:00-06:00` (#436). `off` or an empty text means none.
It is on by default on battery power (`CONFIG_SCHEDULE_QUIET`); on mains there
are none unless `config set quiet …` is used.

- Inside the window the hub neither reads the cameras nor checks in; it sleeps
  until the end (deep sleep, at most `POWER_MAX_SLEEP_MIN`). It overrides both
  schedules, the back-off and the stretches for a low battery.
- What fell due during the window waits and runs at its end: one cycle with a
  camera read (which syncs as well), so the night's media and waiting commands
  are handled together. A read asked for by the app waits too; the app can
  show when the hub is next awake.
- A power-on, a reset and the BOOT button still start a cycle by hand.
- **Read windows at the sun win over the quiet hours.** With the default plan
  in summer the morning window starts before 06:00 (sunrise − 60 min, ~03:45
  in June), and the quiet hours end there; in winter they stay 23:00–06:00. In
  the evening a window that runs past 23:00 keeps the hub awake until it ends.
  Without sun times (no location is no problem: the fallback applies) the
  fixed window holds.
- Without a valid clock the window is ignored, as the read windows are.
- `status` prints `quiet hours … (origin)`.

Not yet there: alignment of reads and check-ins, the
energy factor in the check-in report and the app, and the fields in the
installer and the app (all planned in #436).

### Energy factor

With a battery gauge the hub learns from its own values of the last days
whether it can check in more often (#436). No network and no forecast.

- **Record per local day** (seven days, NVS blob `energy`, written once a day):
  the charge at the first reading of the day (the morning, at rest, so after
  the end of the quiet hours) and the minutes the cell sat at 98 % or more
  while charging (energy that was lost).
- **Factor** for the check-in interval, decided in the morning, in steps 1.0,
  0.75, 0.5: one step towards 0.5 if the morning charge is at least the target
  (`CONFIG_SCHEDULE_ENERGY_TARGET_PERCENT`, 60 %) plus 15 % and the trend (mean
  of the last three day-to-day changes) is not falling, or the cell sat full
  for more than 30 minutes the day before; one step back if the morning is
  below the target or the trend falls faster than 5 % a day. Otherwise it
  stays. Two days of history are needed first.
- **Full cell:** from 95 % the interval is halved at once, for the rest of the
  day (not kept: it follows the charge).
- Not inside quiet hours, not at a low or critical hub battery level, never
  shorter than `CONFIG_SCHEDULE_MIN_INTERVAL_MIN` (10 min), and back-off after
  failed check-ins is applied on top. **Camera reads are not changed**: they
  cost the camera's own battery, which the hub's solar panel does not charge.
- A morning charge that jumps by more than 30 % from the day before means a
  new cell: the history starts again. Power-on or reset keeps the history
  (NVS); the day's running count of full minutes is kept across deep sleep only.
- `status` prints `energy factor 0.75 (morning 82 %, trend +4 %/day)`.

### Where the settings come from

In this order of precedence:

1. **Backend** (`config` in the check-in response): set in the app per hub.
   Removed in the app, the hub falls back to the next source.
2. **Hub** (NVS, `hub_config`): `config set camera_plan …` and
   `config set checkin_min …` on the serial console or in the web installer's
   *Device setup*.
3. **Build** (Kconfig), where the mode runs unattended (see the table above).

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
- The **BOOT button** (GPIO0, `CONFIG_HUB_BOOT_BUTTON`, on by default on the
  ESP32-S3 and ESP32 boards without a button of their own, #224) wakes a
  sleeping hub and starts a camera read at once ("fetch now" on site); while
  the hub is awake a short press searches for the cameras and a long press
  stops the search. GPIO0 is a strapping pin: holding the button while
  pressing reset or connecting power starts the bootloader.

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
level is unknown. It also does not answer with the **CAM DIP switch OFF**: the
log then shows `i2c.master: probe device timeout` and the warning names the I2C
bus and the switch (`docs/hardware.md`, #412). Without a level the check-in
leaves out `hubBatPercent`, so no battery history is stored. The reported percentage is the gauge's own state of charge
(ModelGauge, less affected by the modem's load peaks than the voltage). With
`CONFIG_HUB_BATTERY_PERCENT_FROM_VOLTAGE` it is computed from the cell voltage
instead, along a typical Li-ion curve between `CONFIG_HUB_BATTERY_EMPTY_MV`
(3300) and `CONFIG_HUB_BATTERY_FULL_MV` (4200). The gauge's charge rate
(`CRATE`, register `0x16`) tells whether the cell is being charged (at least
+0.5 %/h, e.g. by the solar panel); `status` and the log show it.

### Saving power on a low hub battery

Between cycles the scheduler reads the gauge and puts the hub into one of
three levels (pure logic in `schedule_plan`, host-tested):

| Level        | Entered below                                                                | Left at          | Effect                                                                                                                                              |
| ------------ | ---------------------------------------------------------------------------- | ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| **low**      | `HUB_BATTERY_LOW_PERCENT` (30 %)                                             | 35 %             | camera reads ×`LOW_CAMERA_FACTOR` (3), check-ins ×`LOW_CHECKIN_FACTOR` (2); not while charging                                                      |
| **critical** | `HUB_BATTERY_CRITICAL_PERCENT` (10 %) or `HUB_BATTERY_CRITICAL_MV` (3500 mV) | 15 % and 3550 mV | no camera reads (also none asked for by the app; they wait), check-ins every `CRITICAL_CHECKIN_MIN` (12 h), no back-off on top; also while charging |

- The hysteresis (`HUB_BATTERY_HYSTERESIS_PERCENT`, 5 %; 50 mV for the
  voltage) keeps the schedule from flipping near a threshold. A hub leaving
  the critical level goes to low first if it is still below 35 %.
- The low stretch multiplies with the back-off and the camera battery
  stretch, up to one day.
- **Modem:** the A7670E draws peaks of up to ~2 A while it transmits. Below
  `HUB_BATTERY_MODEM_MIN_MV` (3400 mV, read right before attaching) a sync
  over the modem is not started and counts as a failed sync. The limit lies
  below the critical voltage on purpose: a hub in the critical level can
  still report it, and only stays silent once the cell is nearly empty.
- **Visible:** the check-in's `schedule` block carries `stretch`
  (`hubBatteryCritical`, `hubBatteryLow` or `cameraBatteryLow`, the most
  important one), the app's schedule card shows it under "Saving power", and
  `status` prints `hub battery level …` and `schedule stretched: …`.
- An unknown battery (no gauge, no cell) changes nothing.
- The level is kept in RTC memory across deep sleep (for the hysteresis);
  after a power-on it starts at normal and is set by the first reading.
- The MAX17048 has no temperature input. Charging below 0 °C stays open
  (#138).


## Not yet covered

- The A7670E modem uplink (#54) is not used; the check-in still joins the home
  Wi-Fi. Its power policy (attach only for check-ins) belongs there.
- Per-camera schedules: the schedule is one shared cycle for all cameras of a
  hub (a scan reads every camera that answers, see "Several cameras per scan"
  below); a read request for any camera triggers a scan of all of them.
- Notifications about a low camera battery (#58).
