#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rebrand_images.py - AppConnect's CarLife art redrawn as Android Auto, in the stock geometry.

    python3 -I kzb-re/rebrand_images.py <stock res/VW_MP2017.kzb> <out.kzb> [--report out.json]

Replaces rebrand_aa.py's URL remap. That pointed the CarLife tile at the stock Android Auto
tile art, which is drawn for another layout: its button fills the whole 196x122 canvas (rows
1-75, full width) with an opaque dark band below it, where every stock tile draws a 70-row
button from row 6, 180 wide, left or right in the canvas, and a faint reflection under it. On
the unit the Android Auto tile came out taller than the MirrorLink one beside it (step 3 run
42), and the welcome screen kept its "Baidu CarLife" logo (a separate image).

What it does (and only this):
  * Each CarLife image below gets new pixels; nothing else changes (no URL, texture, material,
    node or name), so every screen that showed CarLife art now shows Android Auto art.
  * Tiles (N, P, P_Glow; left and "-5" right layouts): the stock CarLife tile keeps its frame,
    reflection and transparent margins; only the button's interior (inside the frame lines) is
    replaced by the stock Android Auto tile's interior of the same state, resampled to the
    stock interior (scale about 0.92 on both axes). Every frame line is checked first.
  * Welcome-screen logo (Node_NotConnect/Plane_BaiduCarlife, 172x59): the stock Android Auto
    logo (Mat_AppConnect_Android_Auto, the same dim style as the CarPlay and MirrorLink logos
    beside it) at its own size, centred where the CarLife logo was. The large bright CarLife
    logo (226x213) gets the same logo at the CarLife logo's brightness.
  * Each image entry is stored as stock stores them: 4 zero bytes, then the whole PNG, with the
    FILE record's size the PNG's length (Kanzi reads that many bytes after the zero bytes). The
    new entry is appended to the data region and only its FILE record is repointed (as
    rebrand_aa.py does); all stock bytes stay where they were.
  * Not changed: the 392x122 "big" CarLife tiles of the Node_Connect/CarLife node (a layout
    for a unit coded with CarLife alone; not seen on this unit).

Needs Pillow. Run with python3 -I on firmware files. Never modifies its input.
"""
import importlib.util
import io
import json
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


kt = _load("kzb_tool")
ra = _load("rebrand_aa")  # model helpers and the stock md5 list

IMG = "/Resource Files/Images/Mat_AppConnect_"

# Interiors (x0, y0, x1, y1; x1/y1 exclusive) and the frame pixels that bound them, as
# (x, y, grey level): measured on the unit's kzb (f394d0bc), checked before anything is drawn.
N_LEFT = ((0, 7, 179, 75), [(179, 20, 130), (60, 6, 129), (60, 75, 132)])
N_RIGHT = ((17, 7, 195, 75), [(16, 20, 130), (195, 20, 132), (60, 6, 129), (60, 75, 132)])
P_LEFT = ((3, 9, 177, 74), [(1, 20, 231), (178, 20, 231), (60, 7, 231), (60, 75, 231)])
P_RIGHT = ((19, 9, 193, 74), [(17, 20, 231), (194, 20, 231), (60, 7, 231), (60, 75, 231)])
AA_N = ((2, 2, 195, 75), [(1, 20, 131), (195, 20, 132), (60, 1, 129), (60, 75, 132)])
AA_P = ((3, 3, 193, 74), [(1, 20, 231), (194, 20, 231), (60, 1, 231), (60, 75, 231)])

TILES = [
    # (CarLife image replaced, its frame, Android Auto image, its frame)
    ("AnBaiduCarLife-N.png", N_LEFT, "Android_Auto-N.png", AA_N),
    ("BaiduCarLife-P.png", P_LEFT, "Android_Auto-P.png", AA_P),
    ("AnBaiduCarLife-P_Glow.png", N_LEFT, "Android_Auto-P_Glow.png", AA_N),
    ("AnBaiduCarLife-5_N.png", N_RIGHT, "Android_Auto-N.png", AA_N),
    ("BaiduCarLife-5_P.png", P_RIGHT, "Android_Auto-P.png", AA_P),
    ("AnBaiduCarLife-5_P_Glow.png", N_RIGHT, "Android_Auto-P_Glow.png", AA_N),
]
LOGO_SRC = "Android_Auto.png"        # 226x213, dim (alpha <= 115) like CarPlay / MirrorLink
LOGO_WELCOME = "BaiduCarlife_2.png"  # 172x59, the welcome screen
LOGO_BRIGHT = "BaiduCarlife.png"     # 226x213, bright (alpha 255)


def grey(p):
    return (p[0] + p[1] + p[2]) // 3


def check_frame(im, frame, what):
    _, marks = frame
    px = im.load()
    for x, y, level in marks:
        g, a = grey(px[x, y]), px[x, y][3]
        if a != 255 or abs(g - level) > 3:
            sys.exit("%s: frame pixel (%d,%d) is %d/%d, expected %d/255" % (what, x, y, g, a, level))


def tile(base, base_frame, aa, aa_frame, what):
    check_frame(base, base_frame, what)
    check_frame(aa, aa_frame, what + " (Android Auto art)")
    bx0, by0, bx1, by1 = base_frame[0]
    inner = aa.crop(aa_frame[0]).resize((bx1 - bx0, by1 - by0), Image.LANCZOS)
    out = base.copy()
    out.paste(inner, (bx0, by0))
    return out


def alpha_bbox(im, min_alpha=8):
    return im.split()[3].point(lambda v: 255 if v >= min_alpha else 0).getbbox()


def logo_welcome(stock, src):
    """The dim Android Auto logo at its own size, centred on the CarLife logo's box."""
    box = alpha_bbox(src)
    logo = src.crop(box)
    sb = alpha_bbox(stock)
    if logo.size[0] > stock.size[0] - 4 or logo.size[1] > stock.size[1] - 4:
        sys.exit("welcome logo: %s does not fit %s" % (logo.size, stock.size))
    cx, cy = (sb[0] + sb[2]) / 2.0, (sb[1] + sb[3]) / 2.0
    x = int(round(cx - logo.size[0] / 2.0))
    y = int(round(cy - logo.size[1] / 2.0))
    x = min(max(x, 2), stock.size[0] - logo.size[0] - 2)
    out = Image.new("RGBA", stock.size, (0, 0, 0, 0))
    out.paste(logo, (x, y))
    return out


def logo_bright(stock, src):
    """The same logo at the bright CarLife logo's level: alpha scaled to full, its grey."""
    sa = [p for p in stock.getdata() if p[3] > 8]
    level = sum(grey(p) * p[3] for p in sa) // sum(p[3] for p in sa)
    amax = max(p[3] for p in src.getdata())
    out = Image.new("RGBA", src.size, (0, 0, 0, 0))
    out.putdata([(level, level, level, min(255, (p[3] * 255 + amax // 2) // amax)) if p[3] else (0, 0, 0, 0)
                 for p in src.getdata()])
    return out


def png_entry(im):
    """Image entry as stock stores it: 4 zero bytes, then the whole PNG. Its FILE size is the
    PNG's length, len(entry) - 4. (Until 2026-11-15 the IEND CRC was left out under the same
    size, so Kanzi read 4 bytes past each new image: the next entry's zero bytes, or past the
    end of the file for the last one. The unit's libpng let it pass; Debian's libpng 1.6 in
    the RCD330 emulator aborts AppConnect on it.)"""
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=False, compress_level=9)
    png = buf.getvalue()
    assert png.endswith(b"IEND\xaeB`\x82"), "unexpected PNG ending"
    return b"\0\0\0\0" + png


def read_entry(blob):
    assert blob[:4] == b"\0\0\0\0" and blob[4:12] == b"\x89PNG\r\n\x1a\n", "not a stock image entry"
    assert blob.endswith(b"IEND\xaeB`\x82"), "image entry does not end with a whole PNG"
    return Image.open(io.BytesIO(blob[4:])).convert("RGBA")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    report_path = argv[argv.index("--report") + 1] if "--report" in argv else None
    stock = open(src, "rb").read()
    assert ra.md5(stock) in ra.STOCK_MD5, "unexpected input md5 %s" % ra.md5(stock)
    m0 = kt.parse(stock)
    assert kt.build(m0) == stock, "round-trip gate FAILED"
    stock_flat = ra.flatten(m0)
    _, stock_problems = kt.check_structure(m0, verbose=False)

    m = kt.parse(stock)
    order, nxt = ra.gap_bounds(m)
    fis = dict(order)
    img = lambda n: read_entry(ra.blob_gap(stock, fis[IMG + n], nxt)[:fis[IMG + n]["size"] + 4])

    new_images = {}
    for name, frame, aa_name, aa_frame in TILES:
        new_images[name] = tile(img(name), frame, img(aa_name), aa_frame, name)
    new_images[LOGO_WELCOME] = logo_welcome(img(LOGO_WELCOME), img(LOGO_SRC))
    new_images[LOGO_BRIGHT] = logo_bright(img(LOGO_BRIGHT), img(LOGO_SRC))

    region = bytearray(m["data_region"])
    base = m["data_region_offset"]
    edits = []
    for name in sorted(new_images):
        fi = fis[IMG + name]
        old = ra.blob_gap(stock, fi, nxt)
        assert img(name).size == new_images[name].size, name
        new = png_entry(new_images[name])
        e = {"path": IMG + name, "old_offset": fi["offset"], "old_size": fi["size"]}
        assert fi["size"] <= len(old), (name, fi["size"], len(old))
        region += b"\0" * (-len(region) % 4)  # each new entry 4-byte aligned
        fi["offset"] = base + len(region)
        fi["size"] = len(new) - 4
        region += new
        e.update({"new_offset": fi["offset"], "new_size": fi["size"], "pixels": "%dx%d" % new_images[name].size})
        edits.append(e)
    m["data_region"] = bytes(region)

    before = (m["dir_size"], m["num_files"], m["root"]["folder_size"], m["data_region_offset"])
    kt.recompute_sizes(m)
    assert before == (m["dir_size"], m["num_files"], m["root"]["folder_size"], m["data_region_offset"])
    out = kt.build(m)
    assert len(out) == m["total_size"]

    # ---- validation on the written bytes
    m2 = kt.parse(out)
    assert kt.build(m2) == out, "re-parse/build not idempotent"
    _, problems2 = kt.check_structure(m2, verbose=False)
    assert problems2 == stock_problems, "structure check differs from stock"
    assert out[base:len(stock)] == stock[base:], "stock data region bytes changed"
    new_flat = ra.flatten(m2)
    allowed = {"header.total_size"} | {"file:" + e["path"] for e in edits}
    unexpected = [k for k in set(stock_flat) | set(new_flat)
                  if stock_flat.get(k) != new_flat.get(k) and k not in allowed]
    assert not unexpected, unexpected
    order2, nxt2 = ra.gap_bounds(m2)
    fis2 = dict(order2)
    for e in edits:
        back = read_entry(ra.blob_gap(out, fis2[e["path"]], nxt2)[:fis2[e["path"]]["size"] + 4])
        assert list(back.getdata()) == list(new_images[e["path"][len(IMG):]].getdata()), e["path"]
    # Kanzi reads `size` bytes after an image's 4 zero bytes: every image, stock and new, is a
    # whole PNG there, inside the file.
    for p, fi in order2:
        if out[fi["offset"] + 4:fi["offset"] + 12] == b"\x89PNG\r\n\x1a\n":
            end = fi["offset"] + 4 + fi["size"]
            assert end <= len(out) and out[end - 8:end] == b"IEND\xaeB`\x82", "not a whole PNG: " + p
    open(dst, "wb").write(out)

    rep = {"stock": src, "stock_md5": ra.md5(stock), "out": dst, "out_md5": ra.md5(out),
           "out_size": len(out), "edits": edits}
    print("stock  %s  md5=%s  round-trip IDENTICAL" % (src, rep["stock_md5"]))
    for e in edits:
        print("image  %-60s %s  off 0x%x->0x%x  size %d->%d" % (e["path"], e["pixels"], e["old_offset"],
                                                               e["new_offset"], e["old_size"], e["new_size"]))
    print("out    %s  md5=%s  %d bytes" % (dst, rep["out_md5"], len(out)))
    if report_path:
        with open(report_path, "w") as f:
            json.dump(rep, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
