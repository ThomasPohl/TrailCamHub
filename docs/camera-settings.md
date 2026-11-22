# Camera settings (512 / 513)

The camera reports all of its settings in the device info (cmdId 512) and
takes changes with cmdId 513. Everything here comes from captures of the
TrailCam Go app (iPhone, 22.11.2026) against a TC100, firmware 2.3.0.3, BLE
3.4.6, `productId` 278, `subModelName` `LUTRANSTRA` (#197). The captures were
taken with `rvictl` and decoded with the scripts in
[`tools/capture/`](../tools/capture/README.md).

Status: **[verified]** means the value was changed in the app and seen in the
capture; *(assumed)* means it follows from the names or a pattern and was not
checked one by one.

## Reading: cmdId 512

Request `{"cmdId":512,"token":<n>}`, see [`udp-protocol.md`](udp-protocol.md)
§5.3. The response holds the settings below as top-level fields next to the
status and identity fields, plus a `capability` object that says which values
this camera supports.

## Writing: cmdId 513

The request carries **only the fields that change**, plus `token` and `seq`
(the same number as the ARTEMIS AppSeq of the request):

```json
{"cmdId":513,"enableAiFilter":0,"token":13237617,"seq":34}
```

Fields that belong together are sent together: switching the mode sent
`workMode`, `shootMode` and `picResolution` in one request; the detection area
sends all eight vertex fields; the screen lock sends the switch and all four
digits. The next 512 response shows the new values.

### Write names differ from read names

| 512 reports | 513 sets with |
| --- | --- |
| `secureCodeOnOff` | `securityCodeOnOff` |
| `timeUTC` | `utcTime` |

So a writer needs its own read → write mapping; sending a 512 name back is not
enough. All other fields seen so far use the same name both ways.

## Settings

| Field | Values | App (German UI) |
| --- | --- | --- |
| `workMode` | 0 / 1 / 2 | Bewegung (motion) / Periodisch (interval) / Zeitraffer (time-lapse) **[verified]** |
| `shootMode` | 0 / 1 / 2 | photo *(assumed)* / video ("Aufnehmen") / photo and video **[verified for 1, 2]**; time-lapse set 1 |
| `picResolution` | code from `capability.imgResolutions` | 9 = 48 MP, 10 = 60 MP **[verified]**; 0–8 not yet mapped |
| `videoResolution` | code from `capability.videoResolutions` | 1 / 2 / 4 / 5 = 720p / 1080p / 2.7K / 4K, in the order of the app list *(assumed, not checked one by one)* |
| `continuousShootCnt` | 1–5 | photos per trigger **[verified]** |
| `recordDuration` | seconds | video length *(assumed)* |
| `recordAudio` | 0 / 1 | sound off / on **[verified]** |
| `timelapseInterval` | seconds | interval for interval / time-lapse mode *(assumed)* |
| `pirSens` | 0 / 1 / 2 | low / medium / high **[verified]** |
| `pirCoolingTime` | seconds | pause after a trigger **[verified]** |
| `sidePirOnOff` | 0 / 1 | side PIR; reported as 1 although `supportSidePir` is false |
| `workTimeOnOff` | 0 / 1 | working hours off / on **[verified]** |
| `workTimeStartHour`, `workTimeStartMin`, `workTimeEndHour`, `workTimeEndMin` | hour, minute | working hours window, e.g. 18:00–07:59 (may span midnight) |
| `enableAiFilter` | 0 / 1 | detection area off / on; turning the area off sent only `enableAiFilter: 0` *(1 = on assumed)* |
| `detectionAreaVertex0X` … `detectionAreaVertex3Y` | 0–1000 | detection area, see below **[verified]** |
| `infraRedLum` | 1 / 2 / 3 | IR low / medium *(assumed)* / high **[verified for 1, 3]** |
| `osdInfoEnable` | false / true | info bar in the picture off / on **[verified]** |
| `WiFiLedEnable` | false / true | Wi-Fi LED *(assumed)* |
| `dateFormat` | 0 / 1 / 2 | DD/MM/YYYY / MM/DD/YYYY / YYYY/MM/DD **[verified]** |
| `timeFormat` | 0 / 1 / 2 | 1 = 12 h, 2 = 24 h **[verified]**; 0 was reported before the first change and is taken to mean 24 h (factory default) |
| `languageCode` | string from `capability.languages` | e.g. `"en"`; the code is sent, not the list position **[verified]** |
| `micVol`, `spkVol` | percent | microphone / speaker volume (`micVol` **[verified]**) |
| `securityCodeOnOff` (513) / `secureCodeOnOff` (512), `secureCode0` … `secureCode3` | 0 / 1, one digit 0–9 per field | screen lock off / on **[verified]**; the app sends all four digits when turning it on and off |
| `utcTime` (513) / `timeUTC` (512) | Unix seconds | camera clock **[verified]**, see below |
| `cameraNameOnOff`, `cameraName` | 0 / 1, string | camera name in the picture *(assumed)* |
| `longitude`, `latitude` | string | GPS stamp *(assumed)*, empty when unset |
| `pwrFreq` | 0 / 1 | mains frequency 50 / 60 Hz *(assumed)* |
| `recordMode`, `lum` | number | unknown |

### Detection area

One quadrilateral, four corners `detectionAreaVertex0..3X/Y` on a 0–1000
scale per axis (0,0 = top left). The app lets each corner move on its own, so
the area is not necessarily an axis-aligned rectangle; only one area is
possible. Example sent by the app:

```json
{"cmdId":513,"detectionAreaVertex0X":55,"detectionAreaVertex0Y":115,
 "detectionAreaVertex1X":975,"detectionAreaVertex1Y":204,
 "detectionAreaVertex2X":1000,"detectionAreaVertex2Y":912,
 "detectionAreaVertex3X":55,"detectionAreaVertex3Y":1000,"token":77400057,"seq":89}
```

Corners run clockwise from the top left. All eight values 0 in the 512 answer
means no area is set. While the area is edited the app shows the live picture
(258 START_AV … 259 STOP_AV); those stream packets are not ARTEMIS JSON.

### Clock

The app sets the clock with `{"cmdId":513,"utcTime":<s>,...}`. The value is the
**local wall clock** as Unix seconds (sent 09:05 CET, value = 09:05:35 UTC),
and `timeZoneOffset` stays 0. This is the same convention the hub already uses
in the login, see [`clock.md`](clock.md).

## Status and identity (read only)

- Status: `batPercent`, `emergencyBatPercent`, `batType`, `curBatType`,
  `isCharging`, `isSolarCharging`, `temperature` (°C), `sdTotalMB`, `sdFreeMB`,
  `sdCardFormatErr`, `cameraWorkMode` (1 in the capture; meaning unknown, not
  the same as `workMode`), `timeZoneOffset`, `timeDst`.
- Identity: `devId`, `fwVerName`, `fwVerCode`, `bleVerName`, `bleVerCode`,
  `modelName`, `subModelName`, `productId`, `customName` (`"ShouLie"`, meaning
  unknown), `screenType`, `wifiSocType`, `defaultMicVolume`, `defaultSpkVolume`.

## Capability

`capability` lists what this camera supports. Example (same camera):

| Field | Value |
| --- | --- |
| `platform`, `screenType` | `"T23ZN"`, 8 |
| `languages` | `langCode` en, de, fr, es, du (Dutch), it, pl, cn, jp, ru |
| `imgResolutions` | `resolution` 0–10 |
| `videoResolutions` | `resolution` 1, 2, 4, 5 |
| true | `enableAiFilter`, `supportScreen`, `supportLockScreen`, `supportMicVolume`, `supportSpkVolume`, `supportOsdInfo`, `supportWiFiLED`, `supportInfraRed`, `supportOnlineService` |
| false | `supportSidePir`, `supportAutoInfraRed`, `dispDetailVideoResolution` |

The video codes have a gap, so resolution values are codes from a list shared
across models, not positions in the app list; the image codes 0–10 are most
likely the same kind of code that this camera happens to support in full.

For the hub this means: take the allowed values from `capability` instead of
hard-coding them, hide a setting whose `support…` flag is false, and do not
trust the value of such a setting (see `sidePirOnOff`).

## Secrets

`secureCode0` … `secureCode3` is the screen lock code in cleartext, in every
512 answer and in every 513 request that changes the lock. They must not be
logged, shown in the web UI, stored in `camera.json` or sent to a destination
(`json_redact()` already covers the 512 answer, [`udp-protocol.md`](udp-protocol.md)
§5.3). The camera's Wi-Fi password is not in 512; it comes over BLE.

## Open

- `shootMode` 0, `picResolution` 0–8, the video codes one by one,
  `infraRedLum` 2, whether `enableAiFilter: 1` turns the area on.
- `recordMode`, `lum`, `pwrFreq`, `cameraWorkMode`, `customName`,
  `cameraNameOnOff`, GPS.
- The answer to 513 (shape and result field) still has to be written down from
  the captures.
