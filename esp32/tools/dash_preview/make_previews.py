#!/usr/bin/env python3
"""Turn the preview's raw frames into screens.png (contact sheet), one PNG
per screen, and demo.gif (price flash + slide transitions). Needs Pillow and
numpy. Usage: make_previews.py <dir with raw/>"""
import glob
import os
import sys

import numpy as np
from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
SCREENS = ["stocks", "weather", "calendar", "stocks_compact", "calendar_compact",
           "stocks_empty", "card_question", "card_tapped", "card_rows",
           "banner", "update", "calibrate", "takeover"]


def load(name):
    a = np.fromfile(os.path.join(OUT, "raw", name + ".raw"), dtype="<u2")
    a = a.reshape(240, 320)
    v = ((a & 0xff) << 8) | (a >> 8)  # panel byte order -> RGB565
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return Image.fromarray(np.dstack([r, g, b]).astype("uint8"))


def sheet(names, cols, path, scale=2):
    ims = [load(n) for n in names]
    rows = (len(ims) + cols - 1) // cols
    pad = 16
    w = cols * (320 * scale + pad) + pad
    h = rows * (240 * scale + pad) + pad
    canvas = Image.new("RGB", (w, h), (40, 40, 44))
    for i, im in enumerate(ims):
        canvas.paste(im.resize((320 * scale, 240 * scale), Image.LANCZOS),
                     (pad + (i % cols) * (320 * scale + pad),
                      pad + (i // cols) * (240 * scale + pad)))
    canvas.save(path)


def gif(path, scale=1):
    seq = []

    def hold(name, ms):
        seq.append((load(name), ms))

    def frames(prefix, ms):
        for p in sorted(glob.glob(os.path.join(OUT, "raw", prefix + "_*.raw"))):
            seq.append((load(os.path.basename(p)[:-4]), ms))

    hold("stocks", 1200)
    frames("flash", 90)
    hold("stocks_after", 900)
    frames("slide", 40)
    hold("weather", 1500)
    frames("slide2", 40)
    hold("calendar", 1500)
    ims = [im.resize((320 * scale, 240 * scale), Image.LANCZOS) for im, _ in seq]
    ims[0].save(path, save_all=True, append_images=ims[1:],
                duration=[d for _, d in seq], loop=0, optimize=True)


def main():
    for n in SCREENS:
        load(n).resize((640, 480), Image.LANCZOS).save(os.path.join(OUT, n + ".png"))
    sheet(SCREENS, 3, os.path.join(OUT, "screens.png"))
    gif(os.path.join(OUT, "demo.gif"))
    print("wrote", os.path.join(OUT, "screens.png"), "and demo.gif")


if __name__ == "__main__":
    main()
