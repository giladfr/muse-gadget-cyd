#!/usr/bin/env bash
# Render the CYD dashboard on this computer, no board needed: builds the real
# dashboard sources against stub ESP-IDF headers, plays a scripted session
# (data pushes, a swipe, taps, calibration, an update, an image takeover) and
# writes screenshots and an animated GIF.
#
# Usage: tools/dash_preview/run.sh [OUT_DIR]   (default: build-preview)
# Needs a C compiler, python3 with Pillow and numpy.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$HERE/../../main"
OUT="${1:-$HERE/../../build-preview}"
mkdir -p "$OUT/raw"
rm -f "$OUT"/raw/*.raw
SRCS=(dashboard.c dash_draw.c dash_screens.c dash_store.c dash_touch.c
      dash_net.c dash_clock.c dash_backlight.c dash_icons.c dash_assets.c)
${CC:-cc} -std=gnu11 -O1 -g ${PREVIEW_CFLAGS:--fsanitize=address,undefined} \
    -Wall -Wno-unused-parameter -Wno-unused-function \
    -I"$HERE/stubs" -I"$MAIN" -I"$MAIN/dashboard" \
    -include "$HERE/stubs/sdkconfig.h" -include "$HERE/stubs/fake_time.h" \
    "$HERE/preview.c" "${SRCS[@]/#/$MAIN/dashboard/}" "$HERE/cJSON.c" \
    -o "$OUT/preview" -lpthread -lm
(cd "$OUT" && ./preview > preview.log)
python3 "$HERE/make_previews.py" "$OUT"
