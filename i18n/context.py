#!/usr/bin/env python3
"""The translator-context pass over the sheet: evidence packs out, written context back in.

    python3 -I i18n/context.py prepare        writes out/i18n/context/<batch>.jsonl (a row per line)
    python3 -I i18n/context.py merge [--force] reads out/i18n/context/<batch>.out.json

prepare: one pack per batch of sheet groups. Each row carries what the code says about the
phrase (callsites.py, both images): the app, the function using it, how the id was found, the
strings that function names (kzb node paths, prefab names) and the other phrases the same
function uses; a phrase no code reaches carries the evidence of a reached phrase with the same
English text, if any. Rows whose context is already written are left out.

merge: each <batch>.out.json is {"<id>": {"element": ..., "context": ...}}; it fills the sheet's
element and context columns where they are empty (all of them with --force).

Element is one of: title, button, list item, option, tab, label, message, pop-up title,
pop-up button, unit, value, status bar, keyboard, unknown. Context is at most two sentences.
"""
import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
import fw  # noqa: E402

DIR = os.path.join(fw.OUT, "context")
BATCHES = {
    "carmenu-a": ["Car menu"],  # split in two by id below
    "carmenu-b": ["Car menu"],
    "notfound": ["Not found in code"],
    "universal-settings": ["Universal", "Settings"],
    "bt-media-radio": ["Bluetooth", "Media", "Radio", "Camera", "Status bar"],
    "popups-climate-misc": ["Pop-ups (toptip)", "Climate", "Engineering menu", "Software update", "System (launcher)",
                            "AppConnect", "AppConnect (own texts)"],
}
ELEMENTS = ["title", "button", "list item", "option", "tab", "label", "message", "pop-up title", "pop-up button",
            "unit", "value", "status bar", "keyboard", "unknown"]


def load_evidence():
    ev = {}
    for base in fw.BASE_NAMES:
        with open(os.path.join(fw.OUT, "evidence-%s.json" % base), encoding="utf-8") as f:
            for k, recs in json.load(f)["evidence"].items():
                if k == "?":
                    continue
                lst = ev.setdefault(int(k), [])
                for r in recs:
                    r = dict(r, base=base)
                    key = (r["app"], r["func"], r["how"], r["via"])
                    if key not in [(x["app"], x["func"], x["how"], x["via"]) for x in lst]:
                        lst.append(r)
    return ev


def prepare():
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    ev = load_evidence()
    en = {r["id"]: r["en"] for r in rows}
    by_func = {}
    for i, recs in ev.items():
        for r in recs:
            by_func.setdefault((r["app"], r["func"]), set()).add(i)
    first_with_text = {}
    for i in sorted(ev):
        first_with_text.setdefault(en.get(str(i)), i)
    os.makedirs(DIR, exist_ok=True)
    car = sorted(int(r["id"]) for r in rows if r["group"] == "Car menu" and not r["context"])
    half = car[len(car) // 2] if car else 0
    for batch, groups in BATCHES.items():
        pack = []
        for r in rows:
            if r["group"] not in groups or r["context"]:
                continue
            if batch == "carmenu-a" and int(r["id"]) >= half or batch == "carmenu-b" and int(r["id"]) < half:
                continue
            item = {"id": r["id"], "en": r["en"], "zh": r["zh"], "group": r["group"], "apps": r["apps"],
                    **{"in_" + b: r["in_" + b] for b in fw.BASE_NAMES}}
            src = None
            if r["id"].isdigit():
                i = int(r["id"])
                src = i if i in ev else first_with_text.get(r["en"])
                if src is not None and src != i:
                    item["evidence_of_same_text_id"] = src
            if src is not None:
                uses = {}
                for e in ev[src]:
                    u = uses.get((e["app"], e["func"]))
                    if u is None:
                        if len(uses) == 6:
                            continue
                        sib = sorted(by_func.get((e["app"], e["func"]), set()) - {src})[:10]
                        u = uses[(e["app"], e["func"])] = {
                            "app": e["app"], "func": e["func"], "how": [], "via": [],
                            "strings": [s for s in e["strings"] if not s.startswith("/") and "|" not in s][:8],
                            "same_function_phrases": [en.get(str(s)) for s in sib]}
                    if e["how"] not in u["how"]:
                        u["how"].append(e["how"])
                    if e["via"] and e["via"] not in u["via"]:
                        u["via"].append(e["via"])
                item["uses"] = list(uses.values())
            elif r["id"].isdigit():
                item["uses"] = "none: not reached from code in either image"
            pack.append(item)
        with open(os.path.join(DIR, batch + ".jsonl"), "w", encoding="utf-8") as f:
            for item in pack:
                f.write(json.dumps(item, ensure_ascii=False) + "\n")
        print("%-22s %4d rows" % (batch, len(pack)))


def merge(force):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = {r["id"]: r for r in csv.DictReader(f)}
    done = bad = 0
    for name in sorted(os.listdir(DIR)):
        if not name.endswith(".out.json"):
            continue
        with open(os.path.join(DIR, name), encoding="utf-8") as f:
            out = json.load(f)
        for rid, v in out.items():
            r = rows.get(rid)
            if not r or v.get("element") not in ELEMENTS or not v.get("context"):
                bad += 1
                continue
            if force or not r["context"]:
                r["element"], r["context"] = v["element"], v["context"].strip()
                done += 1
    extract.write_sheet(rows)
    left = sum(1 for r in rows.values() if not r["context"])
    print("merged %d rows (%d rejected); %d rows still without context" % (done, bad, left))


def main(argv):
    if argv[:1] == ["prepare"]:
        prepare()
    elif argv[:1] == ["merge"]:
        merge("--force" in argv)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
