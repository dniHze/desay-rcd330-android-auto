#!/usr/bin/env python3
"""Empties VW's own texts in the translation sheet, as the public repository keeps it.

    python3 -I i18n/public_sheet.py [sheet]        (default i18n/strings.csv)

The phrase book's stock English and Chinese (rows 0-1652) and the Chinese of the apps' own
texts are VW's. The public repository's sheet leaves them out; extract.py fills them back in
from one's own stock images, and the checks and the build need them there. Run this before
committing the sheet to the public repository. What stays: every translation, the context and
notes, the English of the apps' own texts (the build finds each text in the apps' code by it),
and our own rows (lit:aa:*) whole.
"""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def strip(src, dst):
    """Writes src's sheet to dst without VW's texts; returns (rows, rows changed)."""
    with open(src, encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f)
        cols, rows = r.fieldnames, list(r)
    changed = 0
    for row in rows:
        if row["id"].isdigit():
            changed += bool(row["en"] or row["zh"])
            row["en"] = row["zh"] = ""
        elif not row["id"].startswith("lit:aa:"):
            changed += bool(row["zh"])
            row["zh"] = ""
    tmp = dst + ".new"
    with open(tmp, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    os.replace(tmp, dst)
    return len(rows), changed


if __name__ == "__main__":
    sheet = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "strings.csv")
    n, changed = strip(sheet, sheet)
    print("%s: %d rows, VW's texts taken out of %d" % (os.path.relpath(sheet), n, changed))
