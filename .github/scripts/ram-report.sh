#!/usr/bin/env bash
# Reports the static RAM of a board's builds: how much of each RAM region the
# linked image occupies (code placed in RAM, .data and .bss) and how much is
# left. What is left of the data RAM is the heap at boot.
#
#   ram-report.sh <board id> <label>=<map file> [<label>=<map file> ...]
#
# The table goes to $GITHUB_STEP_SUMMARY and to stdout. A warning is emitted
# when a region has less free space than its threshold:
#   MIN_FREE_IRAM_PERCENT (default 10): instruction RAM. The link fails when
#     it is full (#209), so this is the same early warning as the 10 % rule
#     for the application partition.
#   MIN_FREE_DRAM_PERCENT (default 25): data RAM. The rest is the heap.
# A failure of esp_idf_size only degrades the report, not the build.
set -euo pipefail

board="$1"
shift
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
report="$(mktemp)"
warnings="$(mktemp)"
trap 'rm -f "$report" "$warnings"' EXIT

{
    echo "### Static RAM: $board"
    echo
    echo "| Build | Region | Used | Total | Free | Free % |"
    echo "|---|---|---:|---:|---:|---:|"
} >"$report"

status=0
for entry in "$@"; do
    label="${entry%%=*}"
    map="${entry#*=}"
    json="$(mktemp)"
    if ! python -m esp_idf_size --format json2 "$map" >"$json" 2>/dev/null; then
        echo "::warning::esp_idf_size failed for $map; no static RAM report for $board ($label)"
        rm -f "$json"
        continue
    fi
    python3 - "$board" "$label" "$json" \
        "${MIN_FREE_IRAM_PERCENT:-10}" "${MIN_FREE_DRAM_PERCENT:-25}" >>"$report" 2>>"$warnings" <<'PY' || status=$?
import json
import sys

board, label, path, min_iram, min_dram = sys.argv[1:6]
try:
    layout = json.load(open(path))["layout"]
except (ValueError, KeyError) as error:
    print(f"::warning::Cannot read the esp_idf_size output for {board} ({label}): {error}",
          file=sys.stderr)
    sys.exit(0)

# DRAM and IRAM are separate regions on the ESP32; the newer chips have one
# region for both (DIRAM). Flash and RTC regions are not RAM the heap uses.
thresholds = {"IRAM": int(min_iram), "DRAM": int(min_dram), "DIRAM": int(min_dram)}
for region in layout:
    name = region.get("name", "")
    total = region.get("total", 0)
    if name not in thresholds or not total:
        continue
    used = region.get("used", 0)
    free = total - used
    percent = free * 100 // total
    print(f"| {label} | {name} | {used} B | {total} B | {free} B | {percent} % |")
    if percent < thresholds[name]:
        print(f"::warning::{board} ({label}): only {percent}% of {name} is free "
              f"(threshold {thresholds[name]}%).", file=sys.stderr)
PY
    rm -f "$json"
done

cat "$report"
# Workflow commands on stdout, after the table.
cat "$warnings"
{
    cat "$report"
    echo
} >>"$summary"
exit "$status"
