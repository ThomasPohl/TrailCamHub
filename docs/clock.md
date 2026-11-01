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
