#!/usr/bin/env python3
"""Builds or refreshes the translation sheet, i18n/strings.csv, from the stock images.

    python3 -I i18n/extract.py            (unpacks the images and runs callsites.py if needed)

One row per phrase of the phrase book (libUIPublic.so; ids 0-1652, the same library in 0317 and
0394), plus the apps' own texts ("lit:<app>:<n>": texts an app holds itself, outside the
phrase book; i18n/literals.py). Those rows are written by hand, one per message (its lines
joined by \\n); this script keeps them and sets in_0317/in_0394 from literals.py.

From the images it sets: id, in_0317, in_0394, apps, en, zh. Everything a person or a pass
wrote (group, element, context, the translations, reviewed, notes) is kept as it is. A new row
gets a first group: "Universal" for a short text used by three or more apps, else its app; an
id no code reaches gets the group of a reached id with the same English text, or "Not found in
code". When the stock English of a row that has translations changes, a note says so.

The sheet: UTF-8 with a BOM (Excel, Numbers and Google Sheets open it as is), one row per line,
a line break inside a text written as \\n. Rows are sorted by group (Universal first, the apps,
the apps' own texts, Not found in code), then app, then id.
"""
import csv
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fw  # noqa: E402

SHEET = os.path.join(HERE, "strings.csv")
LANGS = os.path.join(HERE, "languages.csv")
FIXED = ["id"] + ["in_" + b for b in fw.BASE_NAMES] + ["group", "apps", "element", "context", "en", "zh"]
TAIL = ["reviewed", "notes"]
APPS = {  # package name -> group name
    "appconnect": "AppConnect", "bluetooth": "Bluetooth", "camera": "Camera", "carmenu": "Car menu",
    "climate": "Climate", "engineering": "Engineering menu", "launcher": "System (launcher)",
    "mediaplayer": "Media", "radio": "Radio", "setting": "Settings", "statusbar": "Status bar",
    "toptip": "Pop-ups (toptip)", "update": "Software update",
}
UNIVERSAL, NOT_FOUND = "Universal", "Not found in code"


def own_group(app):
    """The group of an app's own texts (lit:<app>:<n> rows)."""
    return APPS[app] + " (own texts)"


def languages():
    with open(LANGS, encoding="utf-8-sig", newline="") as f:
        return [r for r in csv.DictReader(f)]


def columns():
    return FIXED + [l["code"] for l in languages() if l["code"] not in ("en", "zh")] + TAIL


def esc(b):
    t = b.decode("utf-8")
    if "\\n" in t:
        sys.exit("a stock text holds a literal \\n: %r" % t)
    return t.replace("\n", "\\n")


def read_sheet():
    if not os.path.exists(SHEET):
        return {}
    with open(SHEET, encoding="utf-8-sig", newline="") as f:
        return {r["id"]: r for r in csv.DictReader(f)}


def write_sheet(rows):
    order = [UNIVERSAL] + sorted(APPS.values()) + sorted(own_group(a) for a in APPS) + [NOT_FOUND]

    def key(r):
        g = r["group"]
        gi = order.index(g) if g in order else len(order)
        i = r["id"]
        if i.isdigit():
            return (gi, g, (0, "", (0, int(i), "")))
        _, app, n = i.split(":", 2)  # lit:<app>:<n>, or lit:aa:<name> for our own texts
        return (gi, g, (1, app, (0, int(n), "") if n.isdigit() else (1, 0, n)))
    cols = columns()
    tmp = SHEET + ".new"
    with open(tmp, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator="\n", extrasaction="ignore")
        w.writeheader()
        for r in sorted(rows.values(), key=key):
            w.writerow({c: r.get(c, "") for c in cols})
    os.replace(tmp, SHEET)


def evidence(base):
    path = os.path.join(fw.OUT, "evidence-%s.json" % base)
    if not os.path.exists(path):
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "callsites.py"), base], check=True)
    with open(path, encoding="utf-8") as f:
        return json.load(f)["evidence"]


def main():
    have = fw.present()
    if not have:
        sys.exit("no stock image in place (sysupdate/build_update.py BASES)")
    books = {b: fw.phrasebook(os.path.join(fw.unpack(b), "svp", "lib", "libUIPublic.so")) for b in have}
    ev = {b: evidence(b) for b in have}
    a, b = books[have[0]], books[have[-1]]
    for i in range(min(len(a), len(b))):
        if a[i][1:] != b[i][1:]:
            sys.exit("phrase %d differs between %s and %s: %r / %r" % (i, have[0], have[-1], a[i], b[i]))
    old = read_sheet()
    rows = {}
    apps_of = {}
    for base in have:
        for k, recs in ev[base].items():
            if k != "?":
                apps_of.setdefault(int(k), set()).update(r["app"] for r in recs)
    n = max(len(x) for x in books.values())
    en_apps = {}
    for i in range(n):
        en = (b if i >= len(a) else a)[i][2]
        en_apps.setdefault(en, set()).update(apps_of.get(i, ()))
    reached_group = {}
    for i in sorted(apps_of):
        en = (b if i >= len(a) else a)[i][2]
        reached_group.setdefault(en, i)
    for i in range(n):
        ident, zh, en = (b if i >= len(a) else a)[i]
        apps = sorted(apps_of.get(i, ()))
        if apps:
            if len(en_apps[en]) >= 3 and len(en) <= 30:
                group = UNIVERSAL
            else:
                group = APPS[apps[0]] if len(apps) == 1 else APPS[max(apps, key=lambda x: (x != "launcher", x))]
        elif en in reached_group:
            twin = reached_group[en]
            twin_apps = sorted(apps_of[twin])
            group = UNIVERSAL if len(en_apps[en]) >= 3 and len(en) <= 30 else APPS[twin_apps[0]]
        else:
            group = NOT_FOUND
        rows[str(i)] = {"id": str(i), "group": group, "apps": ", ".join(APPS[x] for x in apps),
                        "en": esc(en), "zh": esc(zh),
                        **{"in_" + x: "yes" if x in books and i < len(books[x]) else "" for x in fw.BASE_NAMES}}
    lits = [r for rid, r in old.items() if rid.startswith("lit:")]
    if lits:
        import literals
        found = {base: literals.coverage(base) for base in have}
        for r in lits:
            rows[r["id"]] = dict(r)
            for base in fw.BASE_NAMES:
                rows[r["id"]]["in_" + base] = "yes" if r["id"] in found.get(base, ()) else ""
    langs = [l for l in columns() if l not in FIXED + TAIL]
    changed = 0
    for rid, r in rows.items():
        o = old.get(rid)
        if not o or rid.startswith("lit:"):
            continue
        for c in ["group", "element", "context"] + langs + TAIL:
            if o.get(c):
                r[c] = o[c]
        if o.get("en") and o["en"] != r["en"] and any(o.get(l) for l in langs):
            r["notes"] = ("stock English changed, was: %s. " % o["en"]) + r.get("notes", "")
            changed += 1
    write_sheet(rows)
    reached = sum(1 for i in range(n) if i in apps_of)
    print("%s: %d phrases (%d reached from code), %d apps' own texts, %d rows kept from the old sheet, %d with changed English"
          % (os.path.relpath(SHEET, fw.WORK), n, reached, len(lits), len(set(old) & set(rows)), changed))


if __name__ == "__main__":
    main()
