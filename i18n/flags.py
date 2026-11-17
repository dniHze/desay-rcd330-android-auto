#!/usr/bin/env python3
"""The language list's flags, drawn in the stock flags' frame.

    python3 -I i18n/flags.py <stock setting VW_MP2017.kzb> <out dir> [--preview preview.png]
                             [--appearance <dir>]   also the Appearance pictures (theme_*, blank)

The Settings app's two stock flags (Mat_Sound_Set_en_US.png, Mat_Sound_Set_en_CN.png) are
39x39 RGBA: a 30x21 flag at (4,9) whose outer 2 pixels are a dark frame (#1A1A1A, corners
softened in the alpha), around a 26x17 picture at (6,11). Each new flag keeps the Chinese
flag's frame pixel for pixel and gets a new picture, drawn 8 times larger and reduced.

Writes <out>/<flag>.png for every flag languages.csv names: us and cn are the stock pictures
(English (US) and Chinese), and the others are drawn: gb (the Union Jack, not used now: the
user chose the stock US flag for English (US) on 2026-11-13), es (Spain, the
civil flag with a simplified coat of arms), de, pt (Portugal, a simplified armillary sphere
and shield), tr, ua, nl, it, pl, cz, se, dk (the Nordic crosses in their own proportions), hr
(the chequy shield, without its crown at this size), hu, ro, fi; Needs Pillow.
"""
import importlib.util
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
KZB_RE = os.path.join(os.path.dirname(HERE), "kzb-re")
INNER = (6, 11, 32, 28)  # x0, y0, x1, y1 (exclusive): 26 x 17
SCALE = 8
W, H = (INNER[2] - INNER[0]) * SCALE, (INNER[3] - INNER[1]) * SCALE


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(KZB_RE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def stock_flags(kzb):
    kt, ra, ri = _load("kzb_tool"), _load("rebrand_aa"), _load("rebrand_images")
    data = open(kzb, "rb").read()
    m = kt.parse(data)
    order, nxt = ra.gap_bounds(m)
    fis = dict(order)
    out = {}
    for code, name in (("us", "Mat_Sound_Set_en_US.png"), ("cn", "Mat_Sound_Set_en_CN.png")):
        fi = fis["/Resource Files/Images/" + name]
        out[code] = ri.read_entry(ra.blob_gap(data, fi, nxt)[:fi["size"] + 4])
    return out


def canvas(color):
    im = Image.new("RGB", (W, H), color)
    return im, ImageDraw.Draw(im)


def star(d, cx, cy, r_out, r_in, fill, rotation=-90):
    pts = []
    for k in range(10):
        r = r_out if k % 2 == 0 else r_in
        a = math.radians(rotation + 36 * k)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=fill)


def gb():
    """The Union Jack (2:1 design stretched to the box, as the stock flags fill theirs)."""
    blue, red, white = (1, 33, 105), (200, 16, 46), (255, 255, 255)
    im, d = canvas(blue)
    u = H / 30.0  # the design's unit (the flag is 60 x 30)
    sx = W / 60.0
    diag = 6 * u
    d.line([(0, 0), (W, H)], fill=white, width=int(round(diag * 1.25)))
    d.line([(0, H), (W, 0)], fill=white, width=int(round(diag * 1.25)))
    # The red saltire, counterchanged: offset clockwise in each quarter.
    rw = 2 * u * 1.25
    k = H / float(W)
    for (x0, y0, x1, y1, ox, oy) in ((0, 0, W / 2, H / 2, 0, 1), (W / 2, H / 2, W, H, 0, -1),
                                     (0, H, W / 2, H / 2, -1, 0), (W / 2, H / 2, W, 0, 1, 0)):
        off = rw / 2
        d.line([(x0 + ox * off, y0 + oy * off), (x1 + ox * off, y1 + oy * off)], fill=red, width=int(round(rw)))
    d.rectangle([W / 2 - 5 * sx * 2, 0, W / 2 + 5 * sx * 2, H], fill=white)
    d.rectangle([0, H / 2 - 5 * u, W, H / 2 + 5 * u], fill=white)
    d.rectangle([W / 2 - 3 * sx * 2, 0, W / 2 + 3 * sx * 2, H], fill=red)
    d.rectangle([0, H / 2 - 3 * u, W, H / 2 + 3 * u], fill=red)
    del k
    return im


def es():
    red, yellow = (170, 21, 27), (241, 191, 0)
    im, d = canvas(red)
    d.rectangle([0, H / 4, W, H * 3 / 4], fill=yellow)
    # The coat of arms, simplified for 6 pixels: a crowned shield at a third of the width.
    cx, top = W * 0.34, H * 0.30
    sw, sh = W * 0.15, H * 0.36
    d.rectangle([cx - sw / 2, top + sh * 0.18, cx + sw / 2, top + sh * 0.75], fill=(173, 21, 25))
    d.pieslice([cx - sw / 2, top + sh * 0.45, cx + sw / 2, top + sh * 1.05], 0, 180, fill=(173, 21, 25))
    d.rectangle([cx - sw * 0.38, top, cx + sw * 0.38, top + sh * 0.16], fill=(198, 156, 40))
    d.rectangle([cx + sw * 0.05, top + sh * 0.25, cx + sw * 0.42, top + sh * 0.6], fill=(241, 191, 0))
    d.rectangle([cx - sw * 0.85, top + sh * 0.2, cx - sw * 0.62, top + sh * 0.95], fill=(200, 200, 200))
    d.rectangle([cx + sw * 0.62, top + sh * 0.2, cx + sw * 0.85, top + sh * 0.95], fill=(200, 200, 200))
    return im


def de():
    im, d = canvas((0, 0, 0))
    d.rectangle([0, H / 3, W, H * 2 / 3], fill=(221, 0, 0))
    d.rectangle([0, H * 2 / 3, W, H], fill=(255, 206, 0))
    return im


def pt():
    green, red, yellow = (0, 102, 0), (255, 0, 0), (255, 255, 0)
    im, d = canvas(red)
    d.rectangle([0, 0, W * 2 / 5, H], fill=green)
    cx, cy, r = W * 2 / 5, H / 2, H * 0.30
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=yellow, width=int(H * 0.07))
    d.line([(cx - r, cy), (cx + r, cy)], fill=yellow, width=int(H * 0.04))
    sw, sh = r * 1.05, r * 1.25
    d.rectangle([cx - sw / 2, cy - sh / 2, cx + sw / 2, cy + sh * 0.25], fill=red)
    d.pieslice([cx - sw / 2, cy - sh * 0.05, cx + sw / 2, cy + sh * 0.6], 0, 180, fill=red)
    d.rectangle([cx - sw * 0.3, cy - sh * 0.32, cx + sw * 0.3, cy + sh * 0.18], fill=(255, 255, 255))
    d.pieslice([cx - sw * 0.3, cy - sh * 0.05, cx + sw * 0.3, cy + sh * 0.4], 0, 180, fill=(255, 255, 255))
    for dx, dy in ((0, -0.15), (-0.16, 0.02), (0.16, 0.02), (0, 0.02), (0, 0.19)):
        x, y = cx + dx * sw, cy + dy * sh
        d.rectangle([x - sw * 0.06, y - sh * 0.06, x + sw * 0.06, y + sh * 0.06], fill=(0, 51, 153))
    return im


def tr():
    red, white = (227, 10, 23), (255, 255, 255)
    im, d = canvas(red)
    g = H  # the design is given in units of the flag's height
    cx, cy = g * 0.5 + W * 0.02, H / 2
    r1, r2 = g * 0.25, g * 0.2
    d.ellipse([cx - r1, cy - r1, cx + r1, cy + r1], fill=white)
    c2 = cx + g * 0.0625
    d.ellipse([c2 - r2, cy - r2, c2 + r2, cy + r2], fill=red)
    star(d, cx + g * 0.333 + g * 0.05, cy, g * 0.125, g * 0.125 * 0.382, white, rotation=180)
    return im


def ua():
    im, d = canvas((0, 87, 183))
    d.rectangle([0, H / 2, W, H], fill=(255, 215, 0))
    return im


def stripes(colors, vertical=False):
    im, d = canvas(colors[0])
    n = len(colors)
    for k, c in enumerate(colors[1:], 1):
        if vertical:
            d.rectangle([W * k / n, 0, W, H], fill=c)
        else:
            d.rectangle([0, H * k / n, W, H], fill=c)
    return im, d


def nordic(field, cross, across, down, inner=None, inner_w=0):
    """A Nordic cross: `across` and `down` are the field's parts (before, cross, after) along the
    width and the height, in the flag's own units; `inner` a narrower cross on top."""
    im, d = canvas(field)
    ux, uy = W / float(sum(across)), H / float(sum(down))
    d.rectangle([across[0] * ux, 0, (across[0] + across[1]) * ux, H], fill=cross)
    d.rectangle([0, down[0] * uy, W, (down[0] + down[1]) * uy], fill=cross)
    if inner:
        m = (across[1] - inner_w) / 2.0
        d.rectangle([(across[0] + m) * ux, 0, (across[0] + across[1] - m) * ux, H], fill=inner)
        d.rectangle([0, (down[0] + m) * uy, W, (down[0] + down[1] - m) * uy], fill=inner)
    return im


def nl():
    return stripes([(174, 28, 40), (255, 255, 255), (33, 70, 139)])[0]


def it():
    return stripes([(0, 146, 70), (255, 255, 255), (206, 43, 55)], vertical=True)[0]


def pl():
    return stripes([(255, 255, 255), (220, 20, 60)])[0]


def cz():
    im, d = stripes([(255, 255, 255), (215, 20, 26)])
    d.polygon([(0, 0), (W / 2, H / 2), (0, H)], fill=(17, 69, 126))
    return im


def se():
    return nordic((0, 106, 167), (254, 204, 0), (5, 2, 9), (4, 2, 4))


def dk():
    return nordic((200, 16, 46), (255, 255, 255), (12, 4, 21), (12, 4, 12))


def fi():
    return nordic((255, 255, 255), (0, 47, 108), (5, 3, 10), (4, 3, 4))


def hr():
    red, white, blue = (255, 0, 0), (255, 255, 255), (23, 23, 150)
    im, d = stripes([red, white, blue])
    # The chequy shield (5 x 5, red first), centred, over the three stripes.
    sw, sh = H * 0.42, H * 0.52
    x0, y0 = W / 2 - sw / 2, H * 0.27
    q = sw / 5
    d.rectangle([x0 - q * 0.15, y0 - q * 0.15, x0 + sw + q * 0.15, y0 + sh + q * 0.15], fill=white)
    for r in range(5):
        for c in range(5):
            d.rectangle([x0 + c * q, y0 + r * (sh / 5), x0 + (c + 1) * q, y0 + (r + 1) * (sh / 5)],
                        fill=red if (r + c) % 2 == 0 else white)
    return im


def hu():
    return stripes([(206, 41, 57), (255, 255, 255), (71, 112, 80)])[0]


def ro():
    return stripes([(0, 43, 127), (252, 209, 22), (206, 17, 38)], vertical=True)[0]


DRAW = {"gb": gb, "es": es, "de": de, "pt": pt, "tr": tr, "ua": ua, "nl": nl, "it": it, "pl": pl, "cz": cz,
        "se": se, "dk": dk, "hr": hr, "hu": hu, "ro": ro, "fi": fi}


def framed(template, picture):
    """The template's frame with a new picture inside it."""
    out = template.copy()
    small = picture.resize((INNER[2] - INNER[0], INNER[3] - INNER[1]), Image.LANCZOS).convert("RGBA")
    out.paste(small, INNER[:2])
    return out


# Settings > Appearance's colour themes (appearance.cpp's kThemes): auto shows the three side by
# side (the car's coding picks one).
THEMES = {"amber": (255, 190, 0), "red": (233, 0, 0), "blue": (30, 40, 255), "green": (75, 168, 46)}
CODED = ("amber", "red", "blue")  # the ones the car's coding can choose: auto's swatch
FRAME = (INNER[0] - 2, INNER[1] - 2, INNER[2] + 2, INNER[3] + 2)  # the flags' frame: 30 x 21


def square(template, rgbs):
    """A square swatch: the flags' frame (2-pixel border, rounded corners) cut to 21 x 21, centred
    where the flags are, filled with vertical stripes of `rgbs` (one colour: plain)."""
    x0, y0, x1, y1 = FRAME
    frame = template.crop(FRAME)
    side = y1 - y0
    left = side // 2 + 1
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(frame.crop((0, 0, left, side)), (0, 0))  # its left border and corners
    sq.paste(frame.crop((frame.width - (side - left), 0, frame.width, side)), (left, 0))  # its right
    inner = side - 4
    fill = Image.new("RGBA", (inner, inner))
    d = ImageDraw.Draw(fill)
    for k, rgb in enumerate(rgbs):
        d.rectangle([k * inner // len(rgbs), 0, (k + 1) * inner // len(rgbs), inner], fill=rgb + (255,))
    sq.paste(fill, (2, 2))
    out = Image.new("RGBA", template.size, (0, 0, 0, 0))
    out.paste(sq, ((x0 + x1 - side) // 2, y0))
    return out


def swatches(template):
    """{theme_<key>: a square swatch in the flags' frame} and {blank: a transparent picture}."""
    out = {"theme_" + key: square(template, [rgb]) for key, rgb in THEMES.items()}
    out["theme_auto"] = square(template, [THEMES[k] for k in CODED])
    out["blank"] = Image.new("RGBA", template.size, (0, 0, 0, 0))
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    kzb, out = argv[0], argv[1]
    os.makedirs(out, exist_ok=True)
    stock = stock_flags(kzb)
    tpl = stock["cn"]
    # The frame must be the one measured: dark 2-pixel border, the picture inside.
    px = tpl.load()
    for x, y in ((5, 20), (4, 20), (32, 20), (33, 20), (15, 9), (15, 10), (15, 28), (15, 29)):
        if px[x, y] != (26, 26, 26, 255):
            sys.exit("stock flag frame differs at (%d,%d): %r" % (x, y, px[x, y]))
    sys.path.insert(0, HERE)
    import extract
    want = [l["flag"] for l in extract.languages() if l["flag"]]
    made = {}
    for code in want:
        if code in stock:
            made[code] = stock[code]
        elif code in DRAW:
            made[code] = framed(tpl, DRAW[code]())
        else:
            sys.exit("flags.py: no picture for %s" % code)
    for code, im in made.items():
        im.save(os.path.join(out, code + ".png"), optimize=False, compress_level=9)
    if "--appearance" in argv:  # Settings > Appearance: the theme swatches and a blank picture
        app = argv[argv.index("--appearance") + 1]
        os.makedirs(app, exist_ok=True)
        for key, im in swatches(tpl).items():
            im.save(os.path.join(app, key + ".png"), optimize=False, compress_level=9)
        print("%s: %s" % (app, " ".join(sorted(swatches(tpl)))))
    if "--preview" in argv:
        order = want
        sheet = Image.new("RGBA", (len(order) * 39 * 6, 39 * 6), (90, 90, 90, 255))
        for k, code in enumerate(order):
            im = made[code]
            big = im.resize((39 * 6, 39 * 6), Image.NEAREST)
            sheet.alpha_composite(big, (k * 39 * 6, 0))
        sheet.save(argv[argv.index("--preview") + 1])
    print("%s: %s" % (out, " ".join(sorted(made))))


if __name__ == "__main__":
    main(sys.argv[1:])
