#!/usr/bin/env bash
# Runs a command and, if it fails, repeats the tail of its output as a GitHub
# Actions error annotation, so the failure reason shows up in the pull request
# (and in the check-run API) without opening the full job log.
#
#   .github/scripts/run-annotated.sh "<title>" <command> [args...]
set -uo pipefail

title="$1"
shift
log="$(mktemp)"
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [[ $status -ne 0 ]]; then
  # Annotations take one line: encode %, CR and LF as the runner expects.
  # Compiler command lines and build progress are left out: one command line
  # alone fills the annotation and hides the error after it.
  message="$(grep -v -E '^(ccache |\[[0-9]+/[0-9]+\] )' "$log" | tail -n 40 |
    sed 's/\x1b\[[0-9;]*m//g' |
    sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk '{printf "%s%%0A", $0}')"
  echo "::error title=${title} failed::${message}"
fi
rm -f "$log"
exit "$status"
