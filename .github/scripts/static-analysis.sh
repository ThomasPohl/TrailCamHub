#!/usr/bin/env bash
# Runs clang-tidy (checks in .clang-tidy) on the firmware files that build
# without ESP-IDF: the sources the host tests compile, without the tests
# themselves and without third-party code.
#
#   static-analysis.sh [BUILD_DIR]
#
# BUILD_DIR (default build-host) is a configured host test build; its
# compile_commands.json supplies the file list and the compiler flags.
set -euo pipefail

build_dir=${1:-build-host}
if [[ ! -f $build_dir/compile_commands.json ]]; then
    cmake -S test/host -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug
fi

mapfile -t sources < <(python3 - "$build_dir/compile_commands.json" <<'PY'
import json
import sys

paths = {entry["file"] for entry in json.load(open(sys.argv[1]))}
for path in sorted(paths):
    if "/test/host/" not in path and "/_deps/" not in path:
        print(path)
PY
)
if [[ ${#sources[@]} -eq 0 ]]; then
    echo "::error::No sources in $build_dir/compile_commands.json"
    exit 1
fi

echo "clang-tidy: ${#sources[@]} files ($(clang-tidy --version | grep -o 'version [0-9.]*'))"
printf '%s\n' "${sources[@]}" |
    xargs -P "$(nproc)" -n 4 clang-tidy -p "$build_dir" --quiet \
        2> >(grep -v -E 'warnings? generated|Processing file' >&2)
echo "clang-tidy: no findings"
