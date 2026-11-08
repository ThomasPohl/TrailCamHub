#!/usr/bin/env bash
# Compares the capabilities that boards.json lists for a board with what its
# build enables (the same Kconfig options as hub_config::capabilities()), so
# that the installer and the docs cannot promise what the image lacks.
#   check-board-capabilities.sh <board id> <path to sdkconfig>
set -euo pipefail
board="$1"
sdkconfig="$2"

enabled() {
  grep -q "^CONFIG_$1=y$" "$sdkconfig"
}

built=()
enabled MODEM_UPLINK && built+=(modem)
enabled ETH_UPLINK_ENABLE && built+=(ethernet)
enabled HUB_BATTERY_MAX17048 && built+=(battery_gauge)
enabled SPIRAM && built+=(psram)
enabled BOARD_TDONGLE && built+=(display button)
enabled WEB_UI_ENABLE && built+=(web_ui)
enabled POWER_DEEP_SLEEP && built+=(deep_sleep)

built_sorted="$(printf '%s\n' "${built[@]}" | sort | tr '\n' ' ')"
# python3, not jq: the ESP-IDF container has python3 for sure.
listed_sorted="$(python3 -c 'import json, sys
for b in json.load(open("boards.json"))["boards"]:
    if b["id"] == sys.argv[1]:
        print("\n".join(b["capabilities"]))' "$board" | sort | tr '\n' ' ')"
if [ "$built_sorted" != "$listed_sorted" ]; then
  echo "::error file=boards.json::Capabilities of $board differ: boards.json lists [${listed_sorted% }], the build has [${built_sorted% }]"
  exit 1
fi
echo "Capabilities of $board: ${built_sorted% }"
