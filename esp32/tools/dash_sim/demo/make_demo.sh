#!/usr/bin/env bash
# Record the feature demo (demo.txt) in the simulator, headless, and turn it
# into docs/dashboard/demo.gif. Needs Python with Pillow. --slow-spi paces
# drawing like the board's 40 MHz bus, so slides and fades take real time.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/../../../build-sim}"
GIF="${1:-$HERE/../../../../docs/dashboard/demo.gif}"
OUT="$OUT" "$HERE/../build.sh" >/dev/null
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && "$OUT/dash_sim" --headless --slow-spi --nvs demo.nvs --script "$HERE/demo.txt" >sim.log 2>&1)
python3 - "$TMP" "$GIF" <<'PY'
import glob, sys
from PIL import Image
tmp, gif = sys.argv[1:]
frames, durs = [], []
for f in sorted(glob.glob(tmp + "/frame_*.bmp"))[1:]:
    im = Image.open(f).convert("RGB")
    if frames and im.tobytes() == frames[-1].tobytes():
        durs[-1] += 40
    else:
        frames.append(im)
        durs.append(40)
durs[-1] += 1500
# One palette for every frame, so the GIF stores only what changed.
W, H = 480, 360
small = [f.resize((W, H), Image.LANCZOS) for f in frames]
step = max(1, len(small) // 16)
mosaic = Image.new("RGB", (W, H * len(small[::step])))
for i, f in enumerate(small[::step]):
    mosaic.paste(f, (0, i * H))
pal = mosaic.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
big = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in small]
big[0].save(gif, save_all=True, append_images=big[1:], duration=durs, loop=0,
            optimize=True, disposal=1)
print("%d frames, %.1f s -> %s" % (len(frames), sum(durs) / 1000, gif))
PY
