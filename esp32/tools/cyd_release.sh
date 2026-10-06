#!/usr/bin/env bash
# Build a signed CYD dashboard image for an over-the-air update.
#
# Usage: tools/cyd_release.sh [OUT_DIR]     (default: release/)
#
# Needs ESP-IDF v6 (see tools/board.sh) and devices/sdkconfig.local with the
# SDK token, or GADGET_SDK_TOKEN in the environment. Writes
# OUT_DIR/cyd-dashboard-v<version>.bin and prints the device.ota command.
# Bump version.txt first: the board skips an image that is not newer.
set -euo pipefail
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT"
OUT="${1:-release}"

if [ ! -f devices/sdkconfig.local ] && [ -n "${GADGET_SDK_TOKEN:-}" ]; then
  printf 'CONFIG_GADGET_SDK_TOKEN="%s"\n' "$GADGET_SDK_TOKEN" > devices/sdkconfig.local
fi
if ! grep -q '^CONFIG_GADGET_SDK_TOKEN="mgst_' devices/sdkconfig.local 2>/dev/null; then
  echo "error: no SDK token. Put CONFIG_GADGET_SDK_TOKEN in devices/sdkconfig.local" >&2
  echo "       (see devices/sdkconfig.local.example) or set GADGET_SDK_TOKEN." >&2
  echo "       An image without it cannot reconnect and would roll back." >&2
  exit 1
fi

VERSION="$(tr -d '[:space:]' < version.txt)"
# A clean build, so changes to the board file and overlays take effect.
rm -rf build-cyd
tools/board.sh cyd build

BIN="build-cyd/muse-gadget.bin"  # signed with the key in sdkconfig.defaults
mkdir -p "$OUT"
DEST="$OUT/cyd-dashboard-v$VERSION.bin"
cp "$BIN" "$DEST"
SIZE=$(wc -c < "$DEST" | tr -d ' ')
SHA=$( (command -v sha256sum >/dev/null && sha256sum "$DEST" || shasum -a 256 "$DEST") | cut -d' ' -f1)
SLOT=$((0x1E0000))
echo
echo "Built $DEST"
echo "  version $VERSION, $SIZE bytes ($((SIZE * 100 / SLOT))% of the OTA slot), sha256 $SHA"
echo
echo "Serve it where the board can reach it (plain HTTP is fine: the image is"
echo "signature-checked before it is installed), then ask Muse to run:"
echo "  device.ota {\"url\": \"http://<host>/$(basename "$DEST")\"}"
