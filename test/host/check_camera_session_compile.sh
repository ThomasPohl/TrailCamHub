#!/bin/sh
# Compiles the ESP-IDF camera session code against the host stubs
# (test/host/esp_stubs) in the build variants that change its code: the CI
# builds it only for the boards, so a variant no board uses would otherwise
# break unnoticed. Usage: test/host/check_camera_session_compile.sh [files...]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
link=$root/components/camera_link
inc="-I$root/test/host/esp_stubs -I$link -I$link/include \
-I$root/components/time_sync/include -I$root/components/time_sync"
files=${*:-$link/camera_protocol.cpp}
status=0
check() {
    name=$1
    shift
    for f in $files; do
        if ! g++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror $inc "$@" "$f" \
            2>"${TMPDIR:-/tmp}/compile_check.log"; then
            echo "FAIL [$name] $f"
            head -30 "${TMPDIR:-/tmp}/compile_check.log"
            status=1
        else
            echo "ok   [$name] $(basename "$f")"
        fi
    done
}
check photos
check videos-all "-DCONFIG_CAMERA_DOWNLOAD_VIDEOS=1" "-DCONFIG_SPIRAM=1"
check videos-on-request "-DCONFIG_CAMERA_DOWNLOAD_VIDEOS=1" \
    "-DCONFIG_CAMERA_VIDEO_ON_REQUEST=1" "-DCONFIG_SPIRAM=1" \
    "-DCONFIG_CAMERA_VIDEO_MAX_PER_SESSION=3"
check discard-and-preview "-DCONFIG_CAMERA_DOWNLOAD_VIDEOS=1" \
    "-DCONFIG_CAMERA_VIDEO_DISCARD_DATA=1" \
    "-DCONFIG_CAMERA_THUMBNAIL_SERIAL_PREVIEW=1" \
    "-DCONFIG_CAMERA_VIDEO_HEARTBEAT_INTERVAL_MS=500"
exit $status
