#!/usr/bin/env bash
# Pair your Mac with Muse as a CYD dashboard gadget and drive the simulator
# from your real Muse: its dashboard.* commands show up in the dash_sim window,
# and taps on card buttons reach Muse as chat messages.
#
#   mac_gadget.sh pair --sdk-token mgst_...   once: Bluetooth setup with the Muse app
#   mac_gadget.sh                             run: simulator + Link session to Muse
#   mac_gadget.sh info | unpair
#
# The Mac becomes its own gadget ("CYD Dashboard (simulator)"), next to your
# CYD: it offers only the dashboard commands, never shell or file access.
# Pairing uses the Linux Device SDK (../../../linux) with its CoreBluetooth
# backend; afterwards everything goes over the internet, Bluetooth is not used.
#
# Needs: macOS, python3, and dash_sim's build deps (brew install sdl2 cjson).
# Allow your terminal app under System Settings > Privacy & Security >
# Bluetooth when macOS asks.
#
# Options: --port N (dash_sim's command port, default 8765), --keep-name (pair
# without renaming the Mac; see below), anything else goes to dash_sim.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="$(cd "$HERE/../../../linux" && pwd)"
STATE="${MUSEGADGET_SIM_HOME:-$HOME/.musegadget-sim}"
VENV="$STATE/venv"
export MUSEGADGET_STATE_DIR="$STATE"
export MUSEGADGET_SOCKET="$STATE/musegadget.sock"

action=run
case "${1:-}" in pair|run|info|unpair) action=$1; shift;; esac
port=8765 token="" keep_name=0 sim_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --sdk-token) token=$2; shift 2;;
    --port) port=$2; shift 2;;
    --keep-name) keep_name=1; shift;;
    *) sim_args+=("$1"); shift;;
  esac
done

mkdir -p "$STATE"
chmod 700 "$STATE"
if [ ! -x "$VENV/bin/musegadget" ] || [ "$SDK/pyproject.toml" -nt "$VENV/.installed" ]; then
  echo "Setting up the Muse gadget SDK in $VENV ..."
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q --upgrade pip
  extra=()
  [ "$(uname)" = Darwin ] && extra=(pyobjc-framework-CoreBluetooth)
  "$VENV/bin/pip" install -q -e "$SDK" ${extra[@]+"${extra[@]}"}
  touch "$VENV/.installed"
fi
mg() { "$VENV/bin/musegadget" "$@"; }

if [ -n "$token" ]; then
  (umask 077; printf '%s\n' "$token" > "$STATE/sdk_token")
fi

case "$action" in
info) mg info; exit;;
unpair) mg unpair; exit;;
pair)
  [ -s "$STATE/sdk_token" ] || {
    echo "Pass --sdk-token mgst_... (from gadgets.muse.ai > Account > SDK tokens)." >&2; exit 1; }
  name="$(mg info | sed -n 's/^BLE name: *//p')"
  # After connecting, the phone reads the Bluetooth device name, which on a
  # Mac is the computer name, and checks it against the gadget's id. So the
  # Mac carries the gadget's name while setup is open, then gets its own back.
  if [ "$(uname)" = Darwin ] && [ $keep_name = 0 ]; then
    old="$(scutil --get ComputerName)"
    echo "Renaming this Mac to $name for pairing (back to \"$old\" afterwards; sudo asks once)."
    sudo scutil --set ComputerName "$name"
    trap 'sudo scutil --set ComputerName "$old"; echo "Mac renamed back to \"$old\"."' EXIT
    echo "If the Muse app shows the old name, turn Bluetooth off and on once."
  fi
  mg pair ${sim_args[@]+"${sim_args[@]}"}
  exit;;
esac

# run
OUT="${OUT:-$HERE/../../build-sim}"
OUT="$OUT" "$HERE/build.sh" >/dev/null
if ! mg info | grep -q 'paired: *yes'; then
  echo "Not paired yet: run $0 pair --sdk-token mgst_... first." >&2
  exit 1
fi
mg dash-sim --port "$port" > "$STATE/musegadget.log" 2>&1 &
svc=$!
trap 'kill $svc 2>/dev/null || true' EXIT
echo "Muse session: log in $STATE/musegadget.log"
"$OUT/dash_sim" --nvs "$STATE/dash_sim_nvs.txt" --listen "$port" \
  --muse-socket "$MUSEGADGET_SOCKET" ${sim_args[@]+"${sim_args[@]}"}
