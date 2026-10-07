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
  if ${TIMEOUT[@]+"${TIMEOUT[@]}"} ./dash_sim --headless --nvs "test_$name.nvs" "$@" \
       --script "$HERE/tests/$name.txt" > "test_$name.log" 2>&1; then
    echo "ok   $name ($(grep -c '^PASS' "test_$name.log") checks)"
  else
    echo "FAIL $name"; grep -E '^FAIL' "test_$name.log" || tail -20 "test_$name.log"; fail=1
  fi
}
run cards
run offline
run calibration --swap-touch --invert-x
# Muse's commands and card taps through `musegadget dash-sim` (mac_gadget.sh).
if python3 "$HERE/test_muse_bridge.py" ./dash_sim > test_muse_bridge.log 2>&1; then
  echo "ok   muse_bridge ($(grep -c '^ok' test_muse_bridge.log) checks)"
else
  echo "FAIL muse_bridge"; grep -E '^FAIL' test_muse_bridge.log || tail -20 test_muse_bridge.log; fail=1
fi
exit $fail
