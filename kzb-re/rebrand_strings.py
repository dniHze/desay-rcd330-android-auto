#!/usr/bin/env python3
"""Relabel the user-visible "Baidu CarLife" / "CarLife" texts as "Android Auto" in AppConnect's
binary and the shared UI-text library, for bind-mounting over the unit's files (step 3).

Only text changes: each string is rewritten inside its own NUL-terminated span, found by its
content (appconnect: whole lines, each exactly once; libUIPublic: every user-visible text naming
CarLife, through the rules below, which must leave no "CarLife" behind); the new text
is never longer, and the rest of the span is filled with spaces (so code that measures the
string sees the new text plus invisible trailing spaces, and code that uses a fixed length
never meets an early NUL), except where noted. Nothing else in either file points into the
middle of a patched text other than the one merged product name noted below (checked against
every pc-relative load, literal and relocation). Identifiers such as Btn_Carlife_Change or CarLifeLayer_Prefab are
not touched. Any mismatch aborts without writing.

usage: python3 -I rebrand_strings.py <stock appconnect> <stock libUIPublic.so> <out dir>
writes <out>/bin/appconnect and <out>/lib/libUIPublic.so
"""
import hashlib
import os
import sys

AA = "Android Auto"

# appconnect: whole-line rewrites (stock text -> new text, [pad]), each found by content as a
# whole NUL-terminated string, exactly once. Texts are separate lines of the pop-ups;
# neighbouring lines are noted where a rewrite leans on them.
APPCONNECT = [
    ("to connect to Apple CarPlay or Baidu CarLife?",
     "to connect to Apple CarPlay or Android Auto?"),
    # The linker merged the product name into this line's tail: references point at "Baidu
    # CarLife" inside it (the name AppConnect shows, e.g. "Connecting through %s"). The new
    # name lands at the same place; padding with NUL ends both texts there, so the name has
    # no trailing space.
    ("使用 Apple CarPlay 还是 Baidu CarLife",
     "使用 Apple CarPlay 还是 Android Auto", b"\0"),
    ("CarLife: No data transfer.",
     "Android Auto: no data."),
    # next line: "device or have the app run in the foreground."
    ("To use CarLife, please unlock the mobile",
     "To use Android Auto, unlock the mobile"),
    ("CarLife：没有数据传输。",
     "Android Auto：没有数据。"),
    ("要使用 CarLife，请取消禁用移动设备或在前台运行应用程序。",
     "要使用 Android Auto，请取消禁用移动设备或在前台运行程序。"),
    ("Baidu CarLife: No data transmission.",
     "Android Auto: No data transmission."),
    ("To use Baidu CarLife, please unlock the",
     "To use Android Auto, please unlock the"),
    ("百度 CarLife：没有数据传输。",
     "Android Auto：没有数据传输。"),
    ("要使用百度 CarLife，请解锁移动设备",
     "要使用 Android Auto，请解锁移动设备"),
    ("Could not connect. Please check if Baidu CarLife",
     "Could not connect. Please check if Android Auto"),
    ("激活了 Baidu CarLife。请注意：为了您的安全，",
     "激活了 Android Auto。请注意：为了您的安全，"),
]

# libUIPublic.so: the product name AppConnect puts into "Connecting through %s" and "mobile
# device supports %s", and the system pop-up texts (their wording and line breaks vary between
# builds). Rules, in order, applied to every user-visible string that names CarLife; Chinese
# keeps a space between CJK and Latin text.
UIPUBLIC = [
    ("通过百度 CarLife", "通过 " + AA),
    ("使用百度 CarLife", "使用 " + AA),
    ("激活了百度 CarLife", "激活了 " + AA),
    ("重新启动百度 CarLife", "重新启动 " + AA),
    ("百度 CarLife", AA),
    ("Baidu\nCarLife", "Android\nAuto"),
    ("Baidu CarLife", AA),
]


def user_texts(data):
    """(offset, bytes) of every NUL-terminated string that is user-visible text naming CarLife:
    not a log format, source path, symbol or bare identifier."""
    import re
    for m in re.finditer(rb"[^\x00]{2,}", bytes(data)):
        t = m.group(0)
        if b"CarLife" not in t or b"(%u|" in t or b"/media/" in t or t.startswith(b"_Z"):
            continue
        if re.fullmatch(rb"[A-Za-z0-9_|.]+", t):
            continue
        yield m.start(), t


def span(data, off):
    end = data.index(b"\0", off)
    return data[off:end]


def put(data, off, old, new, pad=b" "):
    cur = span(data, off)
    if cur != old:
        sys.exit("0x%x: expected %r, found %r" % (off, old, cur))
    if len(new) > len(old):
        sys.exit("0x%x: %r is longer than %r" % (off, new, old))
    data[off:off + len(old)] = new + pad * (len(old) - len(new))
    return "0x%06x %3d -> %3d bytes  %s" % (off, len(old), len(new), new.decode("utf-8"))


def find_whole(data, text):
    """Offset of `text` as a whole NUL-terminated string; it must occur exactly once."""
    needle = b"\0" + text + b"\0"
    hits, i = [], data.find(needle)
    while i >= 0:
        hits.append(i + 1)
        i = data.find(needle, i + 1)
    if len(hits) != 1:
        sys.exit("%r: found %d times as a whole string" % (text, len(hits)))
    return hits[0]


def patch_appconnect(data):
    out = []
    for entry in APPCONNECT:
        old, new = entry[0].encode("utf-8"), entry[1].encode("utf-8")
        out.append(put(data, find_whole(data, old), old, new, *entry[2:]))
    return out


def patch_uipublic(data):
    out = []
    for off, old in list(user_texts(data)):
        new = old
        for a, b in UIPUBLIC:
            new = new.replace(a.encode("utf-8"), b.encode("utf-8"))
        if b"CarLife" in new:
            sys.exit("0x%x: %r still names CarLife" % (off, new))
        out.append(put(data, off, old, new))
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    appc_in, uip_in, out_dir = sys.argv[1:]
    report = []
    for src, rel, fn in ((appc_in, "bin/appconnect", patch_appconnect),
                         (uip_in, "lib/libUIPublic.so", patch_uipublic)):
        stock = open(src, "rb").read()
        data = bytearray(stock)
        lines = fn(data)
        assert len(data) == len(stock)
        changed = sum(1 for a, b in zip(stock, data) if a != b)
        path = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(bytes(data))
        report.append("%s: stock %s -> %s, %d strings, %d bytes changed" % (
            rel, hashlib.md5(stock).hexdigest(), hashlib.md5(bytes(data)).hexdigest(), len(lines), changed))
        report += ["  " + l for l in lines]
    print("\n".join(report))


if __name__ == "__main__":
    main()
