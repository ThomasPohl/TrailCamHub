# TrailCamHub gallery app

The web app for TrailCamHub (React + Vite + TypeScript, plain static files).
Users see every camera of the hubs linked to them and sort their recordings;
an admin sees all hubs. It replaces the former Telegram Mini App (#266).
`src/api/types.ts` mirrors the v1 API of the backend in `backend/`.

## Gallery (`gallery.html`)

The page for the end user who sorts recordings on his laptop (#258):
large thumbnails, one tab per camera with its name, the camera's file names
per recording (a video and the photo taken with it, e.g. `DSCF0023.AVI` and
`DSCF0024.JPG`, are one recording; the mediaIds stay the keys), **Fetch video** and
**Delete** per recording, a check box "Auswählen" per recording to fetch or
delete several at once (#262), "delete all new ones" with a confirmation, and
a status line with the hub's last and next contact. The user renames a camera
with "Umbenennen" next to its name and hides it with "Ausblenden"
(shown again from the list at the bottom; #273).
Cameras the hub heard but does not read yet stand under "Neue Kameras
gefunden" with "Aufnehmen" (and a name) or "Ignorieren"; "Nicht mehr
abfragen" next to a camera's name stops reading it, "Doch aufnehmen" takes it
back (#298). A camera taken in stays visible until the hub has read it: it
gets a tab marked "noch nicht erreicht" and a row in "Kameras verwalten",
without recordings, and can be renamed or dropped there; once read, its tab
opens with the recordings. "Nach Kameras suchen" asks the hub to scan at its next contact
(#308). The texts are German. It can be
installed as an app (manifest, PNG icons of 192 and 512 px plus a maskable
one, and a service worker in `public/`), and with Chrome or Edge it writes finished
videos into a folder chosen once (File System Access API, `src/gallery/folder.ts`),
one sub-folder per camera; other browsers download them.

**Activity** (#443): the button "Aktivität" in the header opens
`ActivityDialog`: a heatmap of weekday × hour of the camera clock (numbers in
the cells, so colour is not the only cue), the events of the last 90 days and a
sentence on the busiest and the quietest hours, for one camera or all. Built
from the recordings the gallery loads anyway (`src/gallery/activity.ts`);
nothing new is stored. Recordings less than a minute apart are one event, and
recordings of a camera whose clock was never set (before 2020) are left out
with a note. `mediaTime` is the camera's wall clock, so the hours are the
hub's local time (`docs/clock.md`). From at least 50 events over 14 days the
dialog suggests a **camera read plan**: up to three windows around the busy hours
(hours with at least 1.5 times the average, a gap of one hour is bridged, also
over midnight) every 30 minutes, 2 hours for the rest, e.g.
`16:00-19:00=30,*=120`. "Plan übernehmen" sets it for the hub like the admin's
schedule editor and keeps the other values. The suggestion uses fixed times
because the backend's plan editor does not take sun-relative bounds
(`sunrise-60..sunrise+90`) yet; it counts the events of all cameras, as the
plan is per hub. A day-by-day export and recognising empty triggers (#442) are
not built; `activityEvents` takes a filter for the latter.

With `VITE_API_BASE_URL` it talks to the backend and signs in with the token
from the setup link that `npm run add-user` in `backend/` prints
(`gallery.html#token=…`, #259). Without it, or with `?demo`, it uses a **demo
backend** (`src/gallery/demo.ts`) whose hub checks in every 20 seconds and then
delivers requested videos (a small text file instead of an MP4) and deletes
what was marked. Locally: `npm run dev`, then open `/gallery.html`.

The PNG icons are drawn from `public/gallery-icon.svg` and
`public/gallery-icon-maskable.svg` (the same picture with a full background and
the camera inside the safe zone):

```bash
rsvg-convert -w 192 -h 192 public/gallery-icon.svg -o public/gallery-icon-192.png
rsvg-convert -w 512 -h 512 public/gallery-icon.svg -o public/gallery-icon-512.png
rsvg-convert -w 512 -h 512 public/gallery-icon-maskable.svg -o public/gallery-icon-maskable-512.png
```

An **admin** (role `admin`, #259) gets a button "Verwaltung" with the admin
view (#264, `src/gallery/AdminView.tsx`): every hub with last and next
contact, battery, firmware, schedule and users, its cameras (battery, SD card,
temperature, last read, "Jetzt lesen"), and the schedule set in the app. Faults
(#265) stand at the top of each hub, the button shows their number
("Verwaltung (2)"). The demo shows it with `?demo=admin`.

The admin view is protected by a second factor (#259): the admin enters the
6-digit code of an authenticator app (TOTP) and gets a 15-minute admin session;
the plain link of an admin only shows his own hubs. In the user list of each
hub the admin can create a **new link** for an ordinary user (the old one
stops working; the new link is shown once) or **lock** the user out. Admin
accounts are changed only with `npm run add-user` in `backend/`. A log at the
bottom lists the last logins and changes. `?demo=admin-code` shows the code
prompt in the demo (code `123456`).

### Empty triggers (#442)

"Leere erkennen" marks recordings that probably show nothing (wind, sun,
shadows). It runs only in the browser: the thumbnails are reduced to a small
grey grid and compared with the nearest pictures of the same camera. A compact
patch that differs from all neighbours means an animal; a brightness change of
the whole picture or scattered pixels (grass) does not. A pixel must differ by
more than the neighbours vary at that spot, so restless grass needs a larger
change than a still path. Pictures of the same series (each less than 10
minutes after the one before) are never compared with each other, however long
the series lasts: an animal that stays in front of the camera would otherwise
be its own background and every picture of it would look empty. A picture
inside a long series is compared with pictures before and after it, even far
away, or gets no verdict until there are some (#456). At most two neighbours
come from any other series, so an empty picture next to a long series of one
animal does not get the animal into its background (#457). The newest
recordings have neighbours before them only; they are judged again when later
pictures come, until three of their neighbours are later ones (#458). The info
bar at the bottom is ignored.

Results are kept in IndexedDB on this device, nothing is sent to the backend.
It is off by default; "Leer markieren" (Selten / Normal / Oft) sets how readily
a recording counts as empty, and "Leere ausblenden" hides the marked ones.
Hidden recordings are never part of a selection that is deleted. In the viewer
a verdict can be corrected (and the correction taken back); the correction
always wins. The thresholds in `emptiness.ts` are not tuned on real pictures
yet (phase 1).

## Development

```bash
cd app
npm install
npm run dev          # http://localhost:5173/gallery.html, demo mode
npm test             # unit and component tests (Vitest + Testing Library)
npm run lint         # ESLint
npm run format:check # Prettier
npm run build        # type check + production build into dist/
```

To use a backend, set its URL at build or dev time:

```bash
VITE_API_BASE_URL=https://api.example.com npm run dev
```

## Deployment

CI (`.github/workflows/app.yml`) lints, type-checks, tests and builds the app on
every pull request that touches `app/`. On pushes to `main`, the web installer
job in `firmware-package.yml` builds the app and publishes it together with the
installer on GitHub Pages under `/app/`:

```
https://thomaspohl.github.io/TrailCamHub/app/gallery.html
```

Both are deployed from one workflow because a repository has exactly one
GitHub Pages site and every deployment replaces it completely.

Build-time configuration comes from repository variables (Settings → Secrets
and variables → Actions → Variables):

| Variable                   | Purpose                                        |
| -------------------------- | ---------------------------------------------- |
| `TRAILCAMHUB_API_BASE_URL` | Backend URL; unset → the app runs in demo mode |

No secrets belong in the app: everything in it is public. Users sign in with
an app token from their set-up link (see above).

## API sketch (draft v1)

All requests carry `Authorization: Bearer <userId>.<secret>`.

| Method | Path                           | Purpose                                             |
| ------ | ------------------------------ | --------------------------------------------------- |
| GET    | `/v1/cameras`                  | Cameras of all hubs linked to the user              |
| GET    | `/v1/cameras/{camId}/media`    | Recordings with thumbnail URLs                      |
| POST   | `/v1/cameras/{camId}/commands` | e.g. `{"type":"requestVideo","mediaId":3987654321}` |
| POST   | `/v1/cameras/{camId}/commands` | `{"type":"readCamera"}`: read at next check-in      |
| PUT    | `/v1/hubs/{hubId}/schedule`    | Camera read plan and check-in interval of a hub     |
