# Notes for Claude Code sessions

TrailCamHub: ESP32 firmware that wakes TC100 trail cameras over BLE, reads
them over their Wi-Fi (PPPP/ARTEMIS), stores media on SD and syncs to an AWS
backend with a Telegram Mini App. Protocol and design notes live in `docs/`.

## Git setup after cloning

Session clones are shallow and single-branch (`git clone --depth 1`), so
`remote.origin.fetch` only covers `main`. A pushed branch then gets no
`origin/<branch>` tracking ref, and the stop hook reports "unpushed commits /
no remote branch" although the branch is on GitHub. Right after cloning:

```bash
git remote set-branches origin '*'
```

The clone stays shallow. Then:

- Create branches from an up-to-date `main` (`git fetch origin main`).
- Push new branches with `git push -u origin <branch>`, and do not filter
  away the push output: check that the upstream was set
  (`git status -sb` shows `...origin/<branch>`).
- Open PRs with `gh api repos/ThomasPohl/TrailCamHub/pulls` (GraphQL, and so
  `gh pr` / `gh issue list`, is not available in these sessions; use the REST
  API).

## History is shared: never rewrite it

- No force pushes to `main` or to any shared branch; no rebase or amend of
  pushed commits. Fix mistakes with new commits.
- No modified timestamps (no `GIT_AUTHOR_DATE` / `GIT_COMMITTER_DATE`, no
  `--date`).
- Take over PRs from forks with a merge commit. Conflicts between fork PRs are
  resolved in a separate `merge/...` PR with a merge commit.

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

  Pure logic belongs in modules without ESP-IDF dependencies so that it can be
  tested here.
- The ESP-IDF build runs only in CI, and CI only compiles. Runtime errors (for
  example missing initialisation in `app_main`) only show on hardware, so
  review start-up and runtime paths explicitly.

Mini App (`app/`): `npm ci`, then `npm run lint`, `npm run typecheck`,
`npm test`, `npm run format:check`. Backend (`backend/`) has its own
`package.json` and tests.

## Time handling

Times that come from the camera (`media_time` / `mediaTime`, `timeUTC`) are
its wall clock read as if it were UTC, not real UTC instants. Convert with
`camera_wall_clock_to_utc()` (firmware) and format with `formatCameraTime()`
(app). See `docs/clock.md`.
