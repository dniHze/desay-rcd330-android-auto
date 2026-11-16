#!/usr/bin/env python3
"""Checks the translation sheet, i18n/strings.csv.

    python3 -I i18n/check.py [--lang uk] [--all]      exit 1 if any error

Errors (must be fixed):
  * placeholders: every %1, %2, ... of the English text, and no other, in each translation, and
    as many % signs as the English has;
  * protected terms (keep_terms.txt) that the English text holds must appear unchanged;
  * a line break count above English + 1; for an app's own text ("lit:" rows), any count but
    the English's, or a line with a placeholder that is not the English's line;
  * a letter the unit's font (FZHT_GB18030, from the unpacked image) has no glyph for, or the
    capital sharp s (missing from that font);
  * leading or trailing spaces that differ from the English text.
Warnings (listed with --all, counted otherwise):
  * longer than 1.4 times the English (by characters; the unit's UI has no fixed widths
    measured yet, so this only points at texts to look at on the screen);
  * empty: the unit shows English there;
  * one English text in one group translated two ways.
"""
import csv
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
import fw  # noqa: E402

PLACEHOLDER = re.compile(r"%\d")
RATIO = 1.4


def keep_terms():
    with open(os.path.join(HERE, "keep_terms.txt"), encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def font_cmap():
    """The code points the unit's UI font maps (format 4 and 12 subtables), or None if the
    images are not unpacked."""
    path = os.path.join(fw.tree("0317"), "svp", "share", "fonts", "FZHT_GB18030-Regular.ttf")
    if not os.path.exists(path):
        return None
    d = open(path, "rb").read()
    n = struct.unpack(">H", d[4:6])[0]
    tabs = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag] = off
    off = tabs[b"cmap"]
    cps = set()
    for i in range(struct.unpack(">H", d[off + 2:off + 4])[0]):
        _, _, so = struct.unpack(">HHI", d[off + 4 + 8 * i:off + 12 + 8 * i])
        s = off + so
        fmt = struct.unpack(">H", d[s:s + 2])[0]
        if fmt == 4:
            seg = struct.unpack(">H", d[s + 6:s + 8])[0] // 2
            ends = struct.unpack(">%dH" % seg, d[s + 14:s + 14 + 2 * seg])
            starts = struct.unpack(">%dH" % seg, d[s + 16 + 2 * seg:s + 16 + 4 * seg])
            for a, b in zip(starts, ends):
                cps.update(range(a, b + 1))
        elif fmt == 12:
            for k in range(struct.unpack(">I", d[s + 12:s + 16])[0]):
                a, b, _ = struct.unpack(">III", d[s + 16 + 12 * k:s + 28 + 12 * k])
                cps.update(range(a, b + 1))
    return cps


def present(term, text):
    return re.search(r"(?<![A-Za-z])" + re.escape(term) + r"(?![A-Za-z])", text) is not None


def main(argv):
    show_all = "--all" in argv
    only = argv[argv.index("--lang") + 1] if "--lang" in argv else None
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    if not any(r["en"] for r in rows if r["id"].isdigit()):
        # The public repository's sheet (public_sheet.py): the checks need the stock English.
        print("The sheet has no stock English: run i18n/extract.py first (it reads your stock images).")
        return 1
    langs = [c for c in extract.columns() if c not in extract.FIXED + extract.TAIL]
    if only:
        langs = [only]
    terms = sorted(keep_terms(), key=len, reverse=True)
    cmap = font_cmap()
    errors, warnings = [], []
    seen = {}
    for r in rows:
        en = r["en"]
        want_ph = sorted(PLACEHOLDER.findall(en))
        # A shorter term inside a longer one (CarLife in Baidu CarLife) is checked through the longer.
        want_terms, rest = [], en
        for t in terms:
            if present(t, rest):
                want_terms.append(t)
                rest = re.sub(r"(?<![A-Za-z])" + re.escape(t) + r"(?![A-Za-z])", " ", rest)
        for lang in langs:
            t = r.get(lang, "")
            where = "%s [%s] %s" % (r["id"], lang, r["group"])
            if not t:
                if en.strip():
                    warnings.append(("empty", where, en))
                continue
            if sorted(PLACEHOLDER.findall(t)) != want_ph or t.count("%") != en.count("%"):
                errors.append(("placeholders", where, "%s -> %s" % (en, t)))
            for term in want_terms:
                if term not in t:
                    errors.append(("term %s" % term, where, "%s -> %s" % (en, t)))
            if t.count("\\n") > en.count("\\n") + 1:
                errors.append(("line breaks", where, "%s -> %s" % (en, t)))
            if r["id"].startswith("lit:"):
                el, tl = en.split("\\n"), t.split("\\n")
                if len(tl) != len(el) or any(PLACEHOLDER.search(a) and a != b for a, b in zip(el, tl)):
                    errors.append(("own text lines", where, "%s -> %s" % (en, t)))
            if "ẞ" in t:
                errors.append(("capital sharp s", where, t))
            if cmap is not None:
                missing = sorted({c for c in t.replace("\\n", "") if ord(c) not in cmap})
                if missing:
                    errors.append(("no glyph %s" % "".join(missing), where, t))
            if (en[:1] == " ") != (t[:1] == " ") or (en[-1:] == " ") != (t[-1:] == " "):
                errors.append(("edge spaces", where, "%r -> %r" % (en, t)))
            if len(en) >= 4 and len(t) > RATIO * len(en):
                warnings.append(("long x%.1f" % (len(t) / len(en)), where, "%s -> %s" % (en, t)))
            k = (lang, r["group"], en)
            if k in seen and seen[k][1] != t:
                warnings.append(("inconsistent", where, "%s: %s / %s (id %s)" % (en, t, seen[k][1], seen[k][0])))
            seen.setdefault(k, (r["id"], t))
    for kind, where, what in errors:
        print("ERROR %-20s %s: %s" % (kind, where, what))
    counts = {}
    for kind, where, what in warnings:
        counts[kind.split()[0]] = counts.get(kind.split()[0], 0) + 1
        if show_all:
            print("warn  %-20s %s: %s" % (kind, where, what))
    if cmap is None:
        print("(font not checked: run i18n/fw.py unpack)")
    print("%d rows, languages %s: %d errors; warnings %s" % (len(rows), ",".join(langs), len(errors),
                                                            ", ".join("%s %d" % kv for kv in sorted(counts.items())) or "none"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
