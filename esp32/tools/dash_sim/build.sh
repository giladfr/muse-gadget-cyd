#!/usr/bin/env bash
# Build the CYD dashboard simulator (dash_sim) from the real dashboard sources.
#
#   tools/dash_sim/build.sh            -> build-sim/dash_sim
#
# Needs a C compiler, SDL2, libcurl and cJSON:
#   macOS:  brew install sdl2 cjson        (curl ships with macOS)
#   Ubuntu: sudo apt install libsdl2-dev libcurl4-openssl-dev libcjson-dev
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$HERE/../../main"
OUT="${OUT:-$HERE/../../build-sim}"
mkdir -p "$OUT"

if pkg-config --exists libcjson 2>/dev/null; then
  CJSON_CFLAGS="$(pkg-config --cflags libcjson)"
  CJSON_LIBS="$(pkg-config --libs libcjson)"
else
  PREFIX="$( (brew --prefix 2>/dev/null) || echo /usr)"
  CJSON_CFLAGS="-I$PREFIX/include"
  CJSON_LIBS="-L$PREFIX/lib -lcjson"
fi
# The firmware includes "cJSON.h"; cJSON installs it as cjson/cJSON.h.
for d in $CJSON_CFLAGS; do
  case "$d" in -I*) [ -f "${d#-I}/cjson/cJSON.h" ] && CJSON_CFLAGS="$CJSON_CFLAGS $d/cjson";; esac
done
[ -f /usr/include/cjson/cJSON.h ] && CJSON_CFLAGS="$CJSON_CFLAGS -I/usr/include/cjson"

SRCS=(dashboard.c dash_draw.c dash_screens.c dash_store.c dash_touch.c dash_net.c
      dash_clock.c dash_backlight.c dash_icons.c dash_assets.c dash_quotes.c
      dash_cards.c dash_events.c)

# Simulator stubs first (FreeRTOS on threads, the real clock), then the
# preview's ESP-IDF header stubs; cJSON before them (the real one).
${CC:-cc} -std=gnu11 -O1 -g ${SIM_CFLAGS:-} -Wall -Wno-unused-parameter \
    -I"$HERE" -I"$HERE/stubs" $CJSON_CFLAGS -I"$HERE/../dash_preview/stubs" \
    -I"$MAIN" -I"$MAIN/dashboard" $(sdl2-config --cflags) $(curl-config --cflags) \
    -include "$HERE/stubs/sdkconfig.h" \
    "$HERE/sim_main.c" "$HERE/sim_platform.c" "$HERE/http_curl.c" \
    "${SRCS[@]/#/$MAIN/dashboard/}" \
    -o "$OUT/dash_sim" $(sdl2-config --libs) $(curl-config --libs) $CJSON_LIBS -lpthread -lm
echo "built $OUT/dash_sim  (run it: $OUT/dash_sim, or --help)"
