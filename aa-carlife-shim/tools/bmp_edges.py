#!/usr/bin/env python3
"""Measure padding in a LayerManagerControl screen dump (BMP): the leading columns/rows that
are pure green (unwritten NV12) or that repeat their neighbour exactly (decoder edge padding),
and so where the picture starts. The first column or row after the green ones can be a blend
of the two (the compositor filters across the boundary): it is skipped, not taken as picture.
usage: bmp_edges.py <file.bmp>..."""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for r in range(abs(h)):
        y = r if h < 0 else abs(h) - 1 - r  # BMP rows are bottom-up unless h < 0
        base = off + y * stride
        rows.append([tuple(d[base + x * bpp // 8 + i] for i in (2, 1, 0)) for x in range(w)])
    return rows


def green(p):
    r, g, b = p
    return g > 100 and r < 60 and b < 60


for path in sys.argv[1:]:
    px = load(path)
    h, w = len(px), len(px[0])
    col = lambda x: [px[y][x] for y in range(h)]
    gcols = next((x for x in range(w) if not all(green(p) for p in col(x))), w)
    grows = next((y for y in range(h) if not all(green(p) for p in px[y])), h)
    def run(first, same, n):
        # Length of the run of lines equal to their successor from `first`, or from the line
        # after it if that run is longer (a blended boundary line).
        length = lambda a: next((i for i in range(a, n - 1) if not same(i)), n - 1) - a
        a, b = length(first), length(first + 1)
        return (0, a) if a >= b else (1, b)

    skip_c, rep_cols = run(gcols, lambda x: col(x) == col(x + 1), w)
    skip_r, rep_rows = run(grows, lambda y: px[y] == px[y + 1], h)
    print("%s: %dx%d  green columns at left %d, green rows at top %d, repeated columns %d, repeated rows %d"
          "%s -> picture at %d,%d"
          % (path, w, h, gcols, grows, rep_cols, rep_rows,
             " (after a blended line)" if skip_c or skip_r else "",
             gcols + skip_c + rep_cols, grows + skip_r + rep_rows))
