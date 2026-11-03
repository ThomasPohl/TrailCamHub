# TrailCamHub app (Telegram Mini App)

The phone UI for TrailCamHub: camera status (battery, SD card, last check-in),
the latest thumbnails, and "download video" requests that the hub picks up at
its next wake-up. Users see every camera of the hubs linked to them; with more
than one camera, a row of chips at the top switches between them (the last
choice is remembered on the device).

It is a plain static web app (React + Vite + TypeScript). Inside Telegram it
runs as a **Mini App**: it uses Telegram's theme colors, the native back
button and haptics, and authenticates with Telegram's signed init data — so
users need no extra account. Opened in a normal browser it runs as a regular
web page, which is the starting point for a later PWA.

## Status

Without `VITE_API_BASE_URL` the app runs in **demo mode** with in-memory sample
data (one hub with two cameras). `src/api/types.ts` mirrors the v1 API of the
backend in `backend/`.

## Development

```bash
cd app
npm install
npm run dev          # http://localhost:5173, demo mode
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
https://thomaspohl.github.io/TrailCamHub/app/
```

Both are deployed from one workflow because a repository has exactly one
GitHub Pages site and every deployment replaces it completely.

Build-time configuration comes from repository variables (Settings → Secrets
and variables → Actions → Variables):

| Variable                   | Purpose                                        |
| -------------------------- | ---------------------------------------------- |
| `TRAILCAMHUB_API_BASE_URL` | Backend URL; unset → the app runs in demo mode |

No secrets belong in the app: everything in it is public. The bot token stays in
the backend, which verifies the init data the app sends.

## Connecting it to a Telegram bot

1. In Telegram, open [@BotFather](https://t.me/BotFather) and create a bot
   with `/newbot` (or pick an existing one).
2. Send `/newapp`, choose the bot and enter the Pages URL above as the Web App
   URL. Alternatively set it as the bot's menu button with `/setmenubutton`.
3. Open the bot and tap the menu button — the app opens inside Telegram.

## API sketch (draft v1)

All requests carry `Authorization: tma <initData>`.

| Method | Path                           | Purpose                                             |
| ------ | ------------------------------ | --------------------------------------------------- |
| GET    | `/v1/cameras`                  | Cameras of all hubs linked to the user              |
| GET    | `/v1/cameras/{camId}/media`    | Recordings with thumbnail URLs                      |
| POST   | `/v1/cameras/{camId}/commands` | e.g. `{"type":"requestVideo","mediaId":3987654321}` |
| POST   | `/v1/cameras/{camId}/commands` | `{"type":"readCamera"}`: read at next check-in      |
| PUT    | `/v1/hubs/{hubId}/schedule`    | Camera read plan and check-in interval of a hub     |
