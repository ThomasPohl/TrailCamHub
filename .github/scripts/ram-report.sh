#!/usr/bin/env bash
# Reports the static RAM of a board's builds: how much of each RAM region the
# linked image occupies (code placed in RAM, .data and .bss) and how much is
# left. What is left of the data RAM is the heap at boot.
#
#   ram-report.sh <board id> <label>=<map file> [<label>=<map file> ...]
#
# The table goes to $GITHUB_STEP_SUMMARY and to stdout. A warning is emitted
# when a region has less free space than its threshold:
#   MIN_FREE_IRAM_PERCENT (default 10): instruction RAM of the ESP32. The
#     link fails when it is full (#209), so this is the same early warning as
#     the 10 % rule for the application partition.
#   MIN_FREE_DRAM_PERCENT (default 25): data RAM (DRAM on the ESP32, the
#     shared region of the newer chips). The rest is the heap.
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
    # esp_idf_size 1.x (ESP-IDF 5.5) has the json2 format only in its new
    # implementation, which the variable selects; 2.x has only that one.
    if ! ESP_IDF_SIZE_NG=1 python -m esp_idf_size --format json2 "$map" \
        >"$json" 2>"$json.err"; then
        echo "::warning::esp_idf_size failed for $map; no static RAM report for $board ($label):" \
            "$(tail -n 3 "$json.err" | tr '\n' ' ')"
        rm -f "$json" "$json.err"
        continue
    fi
    rm -f "$json.err"
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

# Names of the internal RAM regions in esp_idf_size's chip descriptions: DRAM
# and IRAM are separate on the ESP32; the ESP32-S3 and -P4 have one shared
# region (DIRAM) next to small fixed ones; the C series calls it HP SRAM.
# Flash, external RAM and RTC/LP regions are not listed.
ram = [r for r in layout
       if r.get("name") in ("DRAM", "IRAM", "DIRAM", "HP SRAM") and r.get("total")]
if not ram:
    names = ", ".join(str(r.get("name")) for r in layout)
    print(f"::warning::No RAM region in the esp_idf_size output for {board} "
          f"({label}); regions: {names}", file=sys.stderr)
# With a shared region, code and data that do not fit the fixed regions go
# there, so only the shared one says how much is left.
shared = any(r["name"] == "DIRAM" for r in ram)
for region in ram:
    name = region["name"]
    total = region["total"]
    used = region.get("used", 0)
    free = total - used
    percent = free * 100 // total
    print(f"| {label} | {name} | {used} B | {total} B | {free} B | {percent} % |")
    if shared and name != "DIRAM":
        continue
    threshold = int(min_iram) if name == "IRAM" else int(min_dram)
    if percent < threshold:
        print(f"::warning::{board} ({label}): only {percent}% of {name} is free "
              f"(threshold {threshold}%).", file=sys.stderr)
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
