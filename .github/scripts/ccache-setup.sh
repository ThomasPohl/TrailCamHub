#!/usr/bin/env bash
# Sourced by the ESP-IDF build jobs after export.sh: turns on ccache for
# idf.py with its cache in $GITHUB_WORKSPACE/.ccache, which the workflow
# restores and saves with actions/cache.
#
# The dev and release images of a board differ in their sdkconfig, so they
# cannot share objects within a run; the cache makes the next run of each
# variant recompile only what changed.
#
# Paths below the workspace are rewritten relative to the build directory
# (CCACHE_BASEDIR), so a checkout in a subdirectory (the size report's head/
# and base/) hits the objects of a build in the workspace root. ESP-IDF itself
# lives at the same absolute path (/opt/esp/idf) in every job.

if command -v ccache >/dev/null 2>&1; then
    export IDF_CCACHE_ENABLE=1
    export CCACHE_DIR="${GITHUB_WORKSPACE:-$PWD}/.ccache"
    export CCACHE_BASEDIR="${GITHUB_WORKSPACE:-$PWD}"
    export CCACHE_NOHASHDIR=1
    export CCACHE_MAXSIZE=400M
    export CCACHE_COMPRESS=1
    mkdir -p "$CCACHE_DIR"
    ccache --zero-stats >/dev/null
else
    echo "::warning::ccache is not installed in this image; building without a compiler cache"
fi

# Prints the hit rate of this job to the log.
ccache_report() {
    if command -v ccache >/dev/null 2>&1; then
        ccache --show-stats --verbose || ccache --show-stats || true
    fi
}
