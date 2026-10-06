#!/usr/bin/env python3
"""Generate dash_assets.c: anti-aliased Inter fonts and weather icons.

Glyphs and icons are stored as 4-bit alpha (two pixels per byte, row-major,
each glyph starting on a byte boundary) and blended over the strip at draw
time. Re-run after changing sizes or the character sets:

    python3 gen_assets.py /path/to/inter/fonts > ../dash_assets.c

Needs Pillow. Inter is SIL OFL 1.1 (see OFL-Inter.txt).
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter"
DEGREE = 0x7F  # '°' lives in the DEL slot; write "\x7f" in C strings

ASCII = "".join(chr(c) for c in range(32, 127))
FONTS = [
    # name, file, px, charset
    ("small", "Inter-Medium.otf", 12, ASCII),
    ("body", "Inter-SemiBold.otf", 16, ASCII),
    ("large", "Inter-SemiBold.otf", 22, ASCII),
    ("huge", "InterDisplay-SemiBold.otf", 58, " -0123456789"),
]


def pack4(img):
    """'L' image -> 4-bit alpha bytes (two pixels per byte, high nibble first)."""
    w, h = img.size
    px = img.load()
    out, nib = [], []
    for y in range(h):
        for x in range(w):
            nib.append((px[x, y] + 8) // 17)  # 0..255 -> 0..15
    if len(nib) % 2:
        nib.append(0)
    for i in range(0, len(nib), 2):
        out.append((nib[i] << 4) | nib[i + 1])
    return out


def render_font(name, path, size, charset):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs, data = [], []
    for code in range(32, 128):
        ch = "°" if code == DEGREE else chr(code)
        if code != DEGREE and ch not in charset:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        adv = int(round(font.getlength(ch)))
        box = font.getbbox(ch, anchor="ls")  # relative to the baseline
        w, h = box[2] - box[0], box[3] - box[1]
        if w <= 0 or h <= 0:
            glyphs.append((len(data), 0, 0, 0, 0, adv))
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-box[0], -box[1]), ch, font=font, fill=255,
                                 anchor="ls")
        off = len(data)
        data.extend(pack4(img))
        glyphs.append((off, w, h, box[0], ascent + box[1], adv))
    return {
        "name": name, "data": data, "glyphs": glyphs,
        "line_h": ascent + descent, "ascent": ascent,
    }


# ---- icons: drawn at 8x and box-filtered down -----------------------------

SS = 8


def cloud(d, s, ox=0.0, oy=0.0, k=1.0, fill=255):
    def e(cx, cy, r):
        cx, cy, r = (cx * k + ox) * s, (cy * k + oy) * s, r * k * s
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)
    e(5.2, 10.2, 3.0)
    e(9.6, 8.2, 4.1)
    e(12.9, 10.6, 2.6)
    d.rounded_rectangle([(2.2 * k + ox) * s, (10.2 * k + oy) * s,
                         (15.5 * k + ox) * s, (13.2 * k + oy) * s],
                        radius=1.5 * k * s, fill=fill)


def sun(d, s, cx=8.0, cy=8.0, r=3.4, r0=5.0, r1=6.9, w=1.3, fill=255):
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=fill)
    for i in range(8):
        a = i * math.pi / 4
        d.line([((cx + r0 * math.cos(a)) * s, (cy + r0 * math.sin(a)) * s),
                ((cx + r1 * math.cos(a)) * s, (cy + r1 * math.sin(a)) * s)],
               fill=fill, width=int(w * s))


def layers(kind, size):
    """Two alpha layers (primary, secondary) for an icon at `size` px."""
    big = size * SS
    s = big / 16.0
    p = Image.new("L", (big, big), 0)
    q = Image.new("L", (big, big), 0)
    dp, dq = ImageDraw.Draw(p), ImageDraw.Draw(q)
    if kind == "SUN":
        sun(dp, s)
    elif kind == "PARTLY":
        sun(dq, s, 6.0, 6.2, 2.7, 4.0, 5.5, 1.1)
        cloud(dp, s, 1.2, 2.6, 0.92)
        # Knock the sun out where the cloud covers it, with a small gap.
        gap = Image.new("L", (big, big), 0)
        cloud(ImageDraw.Draw(gap), s, 0.9, 2.3, 0.97)
        q.paste(0, mask=gap)
    elif kind == "CLOUD":
        cloud(dp, s, 0.0, 0.6)
    elif kind == "FOG":
        for i, (a, b) in enumerate([(2.5, 13.5), (4, 15), (1, 11), (3.5, 14)]):
            y = 4 + i * 3
            dp.rounded_rectangle([a * s, (y - 0.75) * s, b * s, (y + 0.75) * s],
                                 radius=0.75 * s, fill=255)
    elif kind in ("RAIN", "SNOW", "THUNDER"):
        cloud(dp, s, 0.0, -2.6)
        if kind == "RAIN":
            for (x, y) in [(5.2, 12.0), (9.0, 12.0), (12.8, 12.0),
                           (7.1, 14.6), (10.9, 14.6)]:
                dq.line([(x * s, y * s), ((x - 0.9) * s, (y + 1.5) * s)],
                        fill=255, width=int(1.1 * s))
        elif kind == "SNOW":
            for (x, y) in [(4.8, 12.4), (8.6, 13.2), (12.4, 12.4),
                           (6.7, 15.0), (10.5, 15.0)]:
                r = 0.85
                dq.ellipse([(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s],
                           fill=255)
        else:
            bolt = [(9.6, 7.8), (6.2, 12.4), (8.4, 12.4), (7.0, 16.0),
                    (11.4, 10.6), (9.0, 10.6), (10.8, 7.8)]
            dq.polygon([(x * s, y * s) for x, y in bolt], fill=255)
            # A dark outline around the bolt separates it from the cloud.
            halo = Image.new("L", (big, big), 0)
            ImageDraw.Draw(halo).line([(x * s, y * s) for x, y in bolt + bolt[:1]],
                                      fill=255, width=int(1.4 * s))
            p.paste(0, mask=halo)
            p.paste(0, mask=q)
    return (p.resize((size, size), Image.BOX), q.resize((size, size), Image.BOX))


ICONS = ["SUN", "PARTLY", "CLOUD", "FOG", "RAIN", "SNOW", "THUNDER"]
ICON_SIZES = [56, 24]


def c_bytes(data, indent="    "):
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


def main():
    out = []
    w = out.append
    w("/*")
    w(" * Generated by assets/gen_assets.py -- do not edit.")
    w(" * Inter fonts (SIL OFL 1.1, see assets/OFL-Inter.txt) and weather icons,")
    w(" * as 4-bit alpha.")
    w(" */")
    w('#include "dash_assets.h"')
    w("")
    total = 0
    for name, file, size, charset in FONTS:
        f = render_font(name, "%s/%s" % (FONT_DIR, file), size, charset)
        total += len(f["data"]) + 7 * 96
        w("static const uint8_t s_%s_bits[] = {" % name)
        w(c_bytes(f["data"]))
        w("};")
        w("static const dash_glyph_t s_%s_glyphs[96] = {" % name)
        for g in f["glyphs"]:
            w("    {%d, %d, %d, %d, %d, %d}," % g)
        w("};")
        w("const dash_font_t dash_font_%s = {s_%s_bits, s_%s_glyphs, %d, %d};"
          % (name, name, name, f["line_h"], f["ascent"]))
        w("")
    for size in ICON_SIZES:
        for kind in ICONS:
            p, q = layers(kind, size)
            pd, qd = pack4(p), pack4(q)
            total += len(pd) + len(qd)
            w("static const uint8_t s_icon%d_%s_p[] = {" % (size, kind.lower()))
            w(c_bytes(pd))
            w("};")
            w("static const uint8_t s_icon%d_%s_q[] = {" % (size, kind.lower()))
            w(c_bytes(qd))
            w("};")
        w("const dash_icon_img_t dash_icons%d[DASH_ICON_COUNT] = {" % size)
        for kind in ICONS:
            w("    {%d, s_icon%d_%s_p, s_icon%d_%s_q}," % (size, size, kind.lower(),
                                                       size, kind.lower()))
        w("};")
        w("")
    w("// %d bytes of asset data." % total)
    print("\n".join(out))


if __name__ == "__main__":
    main()
