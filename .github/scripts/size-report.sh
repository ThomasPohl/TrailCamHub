#!/usr/bin/env bash
# Builds the release profile and writes a firmware size report.
#
# Usage: size-report.sh HEAD_DIR [BASE_DIR]
#
# HEAD_DIR and BASE_DIR are checkouts of the project. When BASE_DIR is given,
# it is built with the same profile and the report includes a comparison.
# The report is written to $GITHUB_STEP_SUMMARY (or stdout outside Actions).
# A warning is emitted when the application partition has less than
# MIN_FREE_PERCENT (default 10) percent free space.
set -euo pipefail

head_dir=$(realpath "$1")
base_dir=${2:+$(realpath "$2")}
min_free_percent=${MIN_FREE_PERCENT:-10}
summary=${GITHUB_STEP_SUMMARY:-/dev/stdout}
app_name=trailcamhub_ble_poc

# Builds the release profile of the project in $1. The base revision may not
# have its own release defaults yet; it then uses the head's file so the
# comparison shows the effect of the code changes alone.
build_release() {
    local project=$1
    local defaults="$project/sdkconfig.defaults.release"
    if [[ ! -f $defaults ]]; then
        defaults="$head_dir/sdkconfig.defaults.release"
    fi
    idf.py -C "$project" -B "$project/build-release" \
        -D IDF_TARGET=esp32s3 \
        -D SDKCONFIG="$project/build-release/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$project/sdkconfig.defaults;$defaults" \
        build 2>&1 | tee "$project/build-release.log"
}

# Prints the free space of the smallest app partition in percent, as reported
# by ESP-IDF's partition size check at the end of the build.
free_percent() {
    grep -oE "${app_name}\.bin binary size .* \(([0-9]+)%\) free" "$1" |
        tail -n 1 | grep -oE '[0-9]+%' | tr -d '%' || true
}

image_size() {
    stat -c %s "$1/build-release/${app_name}.bin"
}

code_block() {
    echo '```'
    cat
    echo '```'
}

# Runs esp_idf_size; a failure only degrades the report, not the build.
idf_size() {
    python -m esp_idf_size "$@" 2>&1 || echo "(esp_idf_size $1 failed)"
}

build_release "$head_dir"
head_size=$(image_size "$head_dir")
head_free=$(free_percent "$head_dir/build-release.log")
head_map="$head_dir/build-release/${app_name}.map"
if [[ -n $base_dir ]]; then
    build_release "$base_dir"
    base_size=$(image_size "$base_dir")
    base_free=$(free_percent "$base_dir/build-release.log")
    base_map="$base_dir/build-release/${app_name}.map"
fi

{
    echo "## Firmware size (release profile)"
    echo
    if [[ -n $base_dir ]]; then
        echo "| | Base | This change | Delta |"
        echo "|---|---:|---:|---:|"
        echo "| Application image | ${base_size} B | ${head_size} B |" \
            "$((head_size - base_size)) B |"
        echo "| App partition free | ${base_free}% | ${head_free}% | |"
    else
        echo "| | Size |"
        echo "|---|---:|"
        echo "| Application image | ${head_size} B |"
        echo "| App partition free | ${head_free}% |"
    fi
    echo
    echo "### Memory usage"
    echo
    idf_size "$head_map" | code_block
    if [[ -n $base_dir ]]; then
        echo
        echo "### Memory usage compared with the base revision"
        echo
        idf_size --diff "$base_map" "$head_map" | code_block
        echo
        echo "<details><summary>Per-component comparison</summary>"
        echo
        idf_size --archives --diff "$base_map" "$head_map" |
            code_block
        echo
        echo "</details>"
    else
        echo
        echo "<details><summary>Per-component usage</summary>"
        echo
        idf_size --archives "$head_map" | code_block
        echo
        echo "</details>"
    fi
} >>"$summary"

if [[ -z $head_free ]]; then
    echo "::warning::Could not determine the free application partition space."
elif ((head_free < min_free_percent)); then
    echo "::warning::Only ${head_free}% of the application partition is free" \
        "(threshold ${min_free_percent}%)."
fi
