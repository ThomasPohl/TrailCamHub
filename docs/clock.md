# Clock

The hub has no battery-backed clock, and a session must work without a time.
When a time is known, it is sent to the camera in the login (cmdId 0), as the
local wall clock, because the camera shows what it is given and keeps
`timeZoneOffset` at 0. The zone is `CONFIG_TIME_ZONE` (a POSIX TZ string, MEZ/MESZ
by default).

Sources, from the best to the worst:

| Source | Used when |
| --- | --- |
| SNTP (`CONFIG_TIME_SYNC_SNTP`) | the uplink has an address |
| Backend (uplink) | the backend sends a time |
| Browser (`CONFIG_TIME_SYNC_BROWSER`) | the web UI is open (`POST /api/time?unix=<s>`, every 30 min); refused once SNTP or the backend set the clock |
| Camera (`CONFIG_TIME_SYNC_CAMERA`) | nothing else set the clock; taken from `timeUTC` in the device info (cmdId 512) |

A time is accepted only if it lies between the build date and ten years after
it. The camera time is not trusted: while the source is the camera, the login
carries no time, and a browser, SNTP or backend time replaces it. `/api/status`
shows `clock_set` and `clock_source`.

## Media times

Times that come from the camera (`time` in the media list, stored as
`media_time` in the catalog, and `timeUTC` in the device info) are its wall
clock read as if it were UTC, not real UTC instants. `camera_wall_clock_to_utc()`
turns them into UTC by `CONFIG_TIME_ZONE`.

Thumbnails and media files on the SD card are dated with the recording time.
The FAT driver stores file times as local time, so `media_store` converts the
wall clock to UTC before `utime()` (`media_file_time()`). The file then shows
the camera's wall clock, and `stat()` returns the real instant. Files stored
before this conversion are shifted by the zone offset (1 h in winter, 2 h in
summer).
