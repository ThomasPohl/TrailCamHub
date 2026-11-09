#!/usr/bin/env bash
# Computes the README badges that shields.io cannot read from GitHub itself
# and writes them as shields.io endpoint files (https://shields.io/badges/endpoint-badge).
#
# Usage: badges.sh REPO_DIR OUT_DIR
#
# REPO_DIR is a checkout of main, OUT_DIR the checkout of the `badges` branch.
# OUT_DIR/ota-history.json keeps one sample of the ota.json download count per
# run (newest 60) so that the active-hub estimate can use a time window.
# Needs gh (GH_TOKEN), jq and GITHUB_REPOSITORY (owner/repo).
set -euo pipefail

repo_dir=$(realpath "$1")
out_dir=$(realpath "$2")
repo=${GITHUB_REPOSITORY:?}
now=$(date -u +%s)

# badge FILE LABEL MESSAGE COLOR
badge() {
    jq -n --arg label "$2" --arg message "$3" --arg color "$4" \
        '{schemaVersion: 1, label: $label, message: $message, color: $color}' \
        >"$out_dir/$1"
    echo "$1: $2 | $3 ($4)"
}

# --- Supported boards (boards.json) -----------------------------------------
boards=$(jq '.boards | length' "$repo_dir/boards.json")
badge boards.json "boards" "$boards supported" blue

# --- Host tests (doctest TEST_CASEs under test/host) -------------------------
tests=$(grep -rhoE '^\s*TEST_CASE\b' "$repo_dir/test/host" | wc -l | tr -d ' ')
badge host-tests.json "host tests" "$tests" brightgreen

# --- Firmware size of the latest release (main board, release variant) -------
# The application image against the size of one OTA slot (partitions.csv).
releases=$(gh api "repos/$repo/releases?per_page=100")
asset_size=$(gh api "repos/$repo/releases/latest" \
    --jq '.assets[] | select(.name == "trailcamhub-esp32s3-release.app.bin") | .size' ||
    true)
slot_hex=$(awk -F, '$1 ~ /^ota_0/ {gsub(/ /, "", $5); print $5}' "$repo_dir/partitions.csv")
if [[ -n $asset_size && -n $slot_hex ]]; then
    slot=$((slot_hex))
    free=$(((slot - asset_size) * 100 / slot))
    size_mb=$(awk -v b="$asset_size" 'BEGIN { printf "%.2f", b / 1048576 }')
    color=brightgreen
    ((free < 25)) && color=yellow
    ((free < 10)) && color=red
    badge firmware-size.json "firmware (S3)" "$size_mb MB, $free% free" "$color"
else
    badge firmware-size.json "firmware (S3)" "no release" lightgrey
fi

# --- Active hubs (estimate) --------------------------------------------------
# Hubs read releases/latest/download/ota.json at most once per 24 h
# (docs/ota.md), so the growth of the ota.json download count per day
# approximates the number of hubs online. All releases are summed because
# "latest" moves with every release. Manual `update` commands and people
# opening the file count as well, hence an estimate.
ota_total=$(jq '[.[] | .assets[] | select(.name == "ota.json") | .download_count] | add // 0' \
    <<<"$releases")
history="$out_dir/ota-history.json"
[[ -f $history ]] || echo '[]' >"$history"
jq --argjson t "$now" --argjson c "$ota_total" '(. + [{t: $t, count: $c}]) | .[-60:]' \
    "$history" >"$history.tmp"
mv "$history.tmp" "$history"

# The oldest sample within the last 7 days that is at least ~1 day old.
reference=$(jq -c --argjson now "$now" \
    '[.[] | select($now - .t >= 82800 and $now - .t <= 7 * 86400 + 3600)] | first // empty' \
    "$history")
if [[ -n $reference ]]; then
    ref_t=$(jq '.t' <<<"$reference")
    ref_count=$(jq '.count' <<<"$reference")
    hubs=$(awk -v d="$((ota_total - ref_count))" -v s="$((now - ref_t))" \
        'BEGIN { v = d * 86400 / s; printf "%d", v + 0.5 }')
    badge active-hubs.json "active hubs (est.)" "≈ $hubs" blue
else
    badge active-hubs.json "active hubs (est.)" "collecting data" lightgrey
fi
