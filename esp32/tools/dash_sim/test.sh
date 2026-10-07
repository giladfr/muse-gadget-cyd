#!/usr/bin/env bash
# Build the simulator and run its scripted tests headless (tests/*.txt).
# Each test fails on any "expect" that doesn't hold.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/../../build-sim}"
"$HERE/build.sh" >/dev/null
cd "$OUT"
fail=0
# macOS has no `timeout` unless coreutils is installed (as gtimeout).
TIMEOUT=()
if command -v timeout >/dev/null; then TIMEOUT=(timeout 120)
elif command -v gtimeout >/dev/null; then TIMEOUT=(gtimeout 120); fi
run() {  # name, extra args
  local name=$1; shift
  rm -f "test_$name.nvs"
  if "${TIMEOUT[@]}" ./dash_sim --headless --nvs "test_$name.nvs" "$@" \
       --script "$HERE/tests/$name.txt" > "test_$name.log" 2>&1; then
    echo "ok   $name ($(grep -c '^PASS' "test_$name.log") checks)"
  else
    echo "FAIL $name"; grep -E '^FAIL' "test_$name.log" || tail -20 "test_$name.log"; fail=1
  fi
}
run cards
run offline
run calibration --swap-touch --invert-x
exit $fail
