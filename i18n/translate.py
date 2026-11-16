#!/usr/bin/env python3
"""The DeepL pass over the sheet: request packs out, translations back in.

    python3 -I i18n/translate.py prepare [--lang uk ...] [--claude-drafts]
                                                           writes out/i18n/translate/<batch>.json
    python3 -I i18n/translate.py merge [--force]           reads out/i18n/translate/<batch>.out.json
    python3 -I i18n/translate.py redo-long [--lang uk ...] writes out/i18n/translate/redo<k>.json
    python3 -I i18n/translate.py collect <batch>           <batch>/call<n>.json -> <batch>.out.json
    python3 -I i18n/translate.py review <fixes.json>...    applies a review's fixes that pass the checks
    python3 -I i18n/translate.py fill <done.json>...       fills empty cells with Claude's drafts

redo-long: the short elements (buttons, tabs, options, list items, titles, values, units) whose
translation is over 1.6 times the English and over 14 characters, sent again one text per call
with the row's own context and a length cap; merge takes a redo result only when it is shorter
than the cell (and still passes the checks), noting "deepl redo".

prepare groups the rows still missing a translation into DeepL calls: one call per screen (the
part of the context before its first colon, within a sheet group; at most 50 texts), each with a
context line for DeepL. A text with line breaks is sent as one line; merge breaks it again into
as many lines as the English has, at the spaces that give the most even lines. Rows that are
not translated get their English copied instead: language names, texts with no letters ("-1",
"0,01", "---"), and texts that are only a protected term (keep_terms.txt).

A batch file: {"calls": [{"context": ..., "texts": [...], "ids": [[id, ...], ...]}], "langs": [...]}
(texts deduplicated within a call; ids[i] are the rows of texts[i]). The pass that runs the
calls writes <batch>.out.json: {"<id>": {"<lang>": text}} (or per call, the same shape); or it
saves each DeepL answer as it came, {"translations": [{"targetLang", "translations": [{"text"}]}]},
to <batch>/call<n>.json (n: the call's index) and collect makes <batch>.out.json from them.
glossaryId is set only when every target language has a dictionary in the glossary (GLOSSARY_LANGS;
adding one is a change to the user's DeepL account, so it is theirs to approve); otherwise the
protected terms go by the instructions alone, and check.py holds them.

merge fills empty cells only (all of them with --force) and leaves a note "deepl" on the row.

review: a post-edit pass's fixes, {"<id>": {"<lang>": {"text", "why"}}}, each applied only to a
cell that has a translation and only if the fixed text passes check.py's rules (placeholders and
% signs, protected terms, the cell's line breaks, edge spaces, the font, no capital sharp s);
noted "post-edit <lang>: <why> (review)". The rest are listed and left as they were.

fill: translations Claude writes (the user's choice since 2026-11-15; DeepL's quota had run out
on 2026-11-13), {"<English text>": {"<lang>": text}}, put into every empty cell of that English
text and language that passes the same checks; the row's note lists the languages as "claude:
nl, it, ...". prepare --claude-drafts can still ask DeepL for those cells; merge then puts
DeepL's text in, keeps Claude's in out/i18n/translate/claude-drafts.json ({"<id>": {"<lang>":
text}}), and takes the language off the note. A review pass then compares the two (both are
drafts: DeepL misses senses on short labels, Claude may miss idiom).
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check  # noqa: E402
import extract  # noqa: E402
import fw  # noqa: E402

DIR = os.path.join(fw.OUT, "translate")
PER_CALL = 50
INSTRUCTIONS = [
    "These are user-interface texts of a car head unit (Volkswagen radio). Translate them as concise interface "
    "labels, no longer than needed.",
    "Never translate or alter: Android Auto, Apple CarPlay, CarPlay, Apple, iPhone, iPod, iTunes, Baidu CarLife, "
    "CarLife, Baidu, MirrorLink®, App-Connect, SmartLink, Bluetooth, Skoda, VW, Climatronic, QWERTY, E-MODE, Eco+.",
    "Never translate these abbreviations: USB, SD, AUX, AM, FM, BT, A2DP, AVRCP, HFP, SMS, PIN, VIN, TPMS, ESC, ASR, "
    "ACC, DCC, DSG, ECO, HI, LO, 12V, OK.",
    "Translate feature names of two or more words (Park Assist, Front Assist, Lane Assist, Side Assist, Light "
    "Assist, Easy Open, Rear View) and Start-Stop with the usual term in the target language. Keep ParkPilot.",
    "Keep placeholders such as %1 and %2 exactly as they are. Translate units of measurement with the usual "
    "abbreviation in the target language.",
    "Use the established terminology of car infotainment systems and vehicle settings in the target language.",
]
GLOSSARY = "08a1247f-9404-4f61-a691-2bd7dd5568cb"  # DeepL "desay-aa keep terms": OK, AUX, HI, LO, AM per pair
GLOSSARY_LANGS = ("ES", "DE", "PT-PT", "TR", "UK")  # the pairs it has


CLAUDE = "claude: "


def claude_langs(r):
    for part in r["notes"].split("; "):
        if part.startswith(CLAUDE):
            return [c.strip() for c in part[len(CLAUDE):].split(",") if c.strip()]
    return []


def set_claude_langs(r, codes):
    parts = [p for p in r["notes"].split("; ") if p and not p.startswith(CLAUDE)]
    if codes:
        parts.append(CLAUDE + ", ".join(codes))
    r["notes"] = "; ".join(parts)


def problems(r, code, t, terms, cmap, same_lines_as=None):
    """check.py's rules for one candidate text of a cell."""
    en = r["en"]
    out = []
    if sorted(check.PLACEHOLDER.findall(t)) != sorted(check.PLACEHOLDER.findall(en)) or t.count("%") != en.count("%"):
        out.append("placeholders")
    out += ["term " + term for term in terms if check.present(term, en) and term not in t]
    n = t.count("\\n")
    if (same_lines_as is not None and n != same_lines_as.count("\\n")) or n > en.count("\\n") + 1 or \
            (r["id"].startswith("lit:") and n != en.count("\\n")):
        out.append("line breaks")
    if (en[:1] == " ") != (t[:1] == " ") or (en[-1:] == " ") != (t[-1:] == " "):
        out.append("edge spaces")
    if "ẞ" in t or (cmap is not None and any(ord(c) not in cmap for c in t.replace("\\n", ""))):
        out.append("glyphs")
    return out


def langs():
    return [l for l in extract.languages() if l["deepl"]]


def copy_through(r, terms):
    en = r["en"]
    if not en.strip() or "do not translate" in r["context"].lower():
        return True
    if not re.search(r"[A-Za-z]", en):
        return True
    return en.strip() in terms


def screen(r):
    c = r["context"]
    if r["group"] == extract.NOT_FOUND:
        return None
    head = c.split(":", 1)[0] if ":" in c[:80] else ""
    return head.strip()


def prepare(only, claude=False):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    terms = set(check.keep_terms())
    codes = [l["code"] for l in langs() if not only or l["code"] in only]
    deepl = {l["code"]: l["deepl"] for l in langs()}
    todo = [r for r in rows if any(not r[c] or (claude and c in claude_langs(r)) for c in codes)
            and not copy_through(r, terms)]
    groups = {}
    for r in todo:
        groups.setdefault((r["group"], screen(r)), []).append(r)
    calls = []
    for (group, scr), rs in groups.items():
        rs.sort(key=lambda r: (0, "", int(r["id"])) if r["id"].isdigit() else (1, r["id"].split(":")[1], int(r["id"].split(":")[2])))
        for k in range(0, len(rs), PER_CALL):
            chunk = rs[k:k + PER_CALL]
            if scr:
                ctx = "Volkswagen car radio touch-screen UI. Screen: %s. " % scr
            elif group == extract.NOT_FOUND:
                ctx = "Volkswagen car radio touch-screen UI: assorted texts (radio, media, phone, vehicle settings). "
            else:
                ctx = "Volkswagen car radio touch-screen UI: %s. " % group
            kinds = sorted({r["element"] for r in chunk})
            ctx += "Texts: %s." % ", ".join(kinds)
            texts, ids = [], []
            for r in chunk:
                t = r["en"].replace("\\n", " ")
                t = re.sub(r" {2,}", " ", t)
                if t in texts:
                    ids[texts.index(t)].append(r["id"])
                else:
                    texts.append(t)
                    ids.append([r["id"]])
            calls.append({"group": group, "screen": scr, "context": ctx[:300], "texts": texts, "ids": ids})
    os.makedirs(DIR, exist_ok=True)
    # Batches of about equal size for parallel runners.
    nb = 6
    calls.sort(key=lambda c: (c["group"], c["screen"] or ""))
    total = sum(len(c["texts"]) for c in calls)
    batches, cur, size = [], [], 0
    for c in calls:
        cur.append(c)
        size += len(c["texts"])
        if size >= total / nb and len(batches) < nb - 1:
            batches.append(cur)
            cur, size = [], 0
    if cur:
        batches.append(cur)
    for name in os.listdir(DIR):
        if name.endswith(".json") and not name.endswith(".out.json"):
            os.remove(os.path.join(DIR, name))
    for k, b in enumerate(batches):
        targets = [deepl[c] for c in codes]
        with open(os.path.join(DIR, "batch%d.json" % k), "w", encoding="utf-8") as f:
            json.dump({"langs": targets, "codes": codes,
                       "glossaryId": GLOSSARY if all(t in GLOSSARY_LANGS for t in targets) else None,
                       "formality": "prefer_more", "customInstructions": INSTRUCTIONS, "calls": b},
                      f, ensure_ascii=False, indent=1)
        print("batch%d: %d calls, %d texts, %d characters" % (k, len(b), sum(len(c["texts"]) for c in b),
                                                            sum(len(t) for c in b for t in c["texts"])))
    print("total %d calls, %d texts, %d characters x %d languages" % (len(calls), total,
          sum(len(t) for c in calls for t in c["texts"]), len(codes)))


SHORT = ("button", "pop-up button", "tab", "option", "list item", "value", "status bar", "unit", "title")


def redo_long(only):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    deepl = {l["code"]: l["deepl"] for l in langs() if not only or l["code"] in only}
    calls = []
    for r in rows:
        if r["element"] not in SHORT or "\\n" in r["en"]:
            continue
        e = r["en"]
        long = [c for c in deepl if len(e) >= 3 and len(r[c]) > 1.6 * len(e) and len(r[c]) > 14]
        if not long:
            continue
        cap = max(len(e) + 4, int(len(e) * 1.3))
        calls.append({"group": r["group"], "screen": None, "texts": [e], "ids": [[r["id"]]], "langs": [deepl[c] for c in long],
                      "codes": long, "context": ("Volkswagen car radio UI, %s. %s" % (r["element"], r["context"]))[:300],
                      "customInstructions": INSTRUCTIONS + [
                          "This is a short %s: at most %d characters. Use a short, common wording or a standard "
                          "abbreviation of the target language if needed." % (r["element"], cap)]})
    for name in os.listdir(DIR):
        if name.startswith("redo") and not name.endswith(".out.json"):
            os.remove(os.path.join(DIR, name))
    nb = 6
    for k in range(nb):
        part = calls[k::nb]
        with open(os.path.join(DIR, "redo%d.json" % k), "w", encoding="utf-8") as f:
            json.dump({"glossaryId": GLOSSARY if all(deepl[c] in GLOSSARY_LANGS for c in deepl) else None,
                   "formality": "prefer_more", "calls": part}, f, ensure_ascii=False, indent=1)
    print("%d rows to redo, in %d files" % (len(calls), nb))


def rewrap(text, lines, terms=()):
    """text broken into `lines` lines at spaces, as evenly as possible (the longest line as short
    as it can be), never inside a protected term of several words."""
    for t in terms:
        if " " in t:
            text = text.replace(t, t.replace(" ", "\0"))
    words = [w.replace("\0", " ") for w in text.split(" ")]
    n = len(words)
    if lines <= 1 or n < lines:
        return " ".join(words)
    # best[k][i]: the smallest longest line for words[:i] in k lines, with the cut that gives it.
    width = lambda i, j: sum(len(w) for w in words[i:j]) + (j - i - 1)  # noqa: E731
    INF = float("inf")
    best = [[(INF, 0)] * (n + 1) for _ in range(lines + 1)]
    best[0][0] = (0, 0)
    for k in range(1, lines + 1):
        for i in range(1, n + 1):
            for j in range(k - 1, i):
                if best[k - 1][j][0] < INF:
                    v = max(best[k - 1][j][0], width(j, i))
                    if v < best[k][i][0]:
                        best[k][i] = (v, j)
    cuts, i = [], n
    for k in range(lines, 0, -1):
        j = best[k][i][1]
        cuts.append(" ".join(words[j:i]))
        i = j
    return "\\n".join(reversed(cuts))


def wrap_like(en, t, terms):
    """The translation `t` (one line) broken like the English `en`: a line that is only a
    placeholder (%1, usually a device name) stays a line of its own, and the text between such
    lines gets as many lines as the English has there. Returns (text, exact): exact is False when
    the placeholders came back in another order and the whole text was just wrapped evenly."""
    lines = en.split("\\n")
    if len(lines) == 1:
        return t, True
    lone = [l.strip() for l in lines if re.fullmatch(r"\s*%\d\s*", l)]
    if not lone:
        return rewrap(t, len(lines), terms), True
    parts = re.split(r"\s*(%s)\s*" % "|".join(re.escape(p) for p in lone), t)
    found = parts[1::2]
    if found != lone:
        return rewrap(t, len(lines), terms), False
    # The English as segments between lone placeholders: line counts of the text in between.
    counts, n = [], 0
    for l in lines:
        if l.strip() in lone:
            counts.append(n)
            n = 0
        else:
            n += 1
    counts.append(n)
    out = []
    for k, seg in enumerate(parts[0::2]):
        seg = seg.strip()
        if counts[k] and seg:
            out.append(rewrap(seg, counts[k], terms))
        elif seg:
            out.append(seg)
        if k < len(found):
            out.append(found[k])
    return "\\n".join(out), True


def merge(force):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = {r["id"]: r for r in csv.DictReader(f)}
    terms = set(check.keep_terms())
    codes = [l["code"] for l in langs()]
    filled = copied = 0
    path = os.path.join(DIR, "claude-drafts.json")
    drafts = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    for r in rows.values():
        if copy_through(r, terms):
            for c in codes:
                if force or not r[c]:
                    if r[c] != r["en"]:
                        r[c] = r["en"]
                        copied += 1
    if os.path.isdir(DIR):
        for name in sorted(os.listdir(DIR)):
            if not name.endswith(".out.json"):
                continue
            with open(os.path.join(DIR, name), encoding="utf-8") as f:
                out = json.load(f)
            redo = name.startswith("redo")
            for rid, per in out.items():
                r = rows.get(rid)
                if not r:
                    continue
                if redo:
                    for c, t in per.items():
                        t = re.sub(r"\s*\n\s*", " ", (t or "").strip())
                        if c in codes and t and len(t) < len(r[c]) and all(term in t for term in terms
                                                                             if check.present(term, r["en"])):
                            r[c] = t
                            if "deepl redo" not in r["notes"]:
                                r["notes"] = (r["notes"] + "; deepl redo (shorter)").strip("; ")
                    continue
                n = r["en"].count("\\n") + 1
                for c, t in per.items():
                    if c not in codes or not t:
                        continue
                    if r[c] and c in claude_langs(r):
                        drafts.setdefault(rid, {})[c] = r[c]
                        r[c] = ""
                        set_claude_langs(r, [x for x in claude_langs(r) if x != c])
                    t = re.sub(r"\s*\n\s*", " ", t.strip())
                    if r["en"].startswith(" "):
                        t = " " + t
                    if r["en"].endswith(" "):
                        t = t + " "
                    if n > 1:
                        t, exact = wrap_like(r["en"], t, terms)
                        if not exact and "check line breaks" not in r["notes"]:
                            r["notes"] = (r["notes"] + "; check line breaks (%s)" % c).strip("; ")
                    if force or not r[c]:
                        r[c] = t
                        filled += 1
                if "deepl" not in r["notes"]:
                    r["notes"] = ("deepl; " + r["notes"]).strip("; ") if r["notes"] else "deepl"
    extract.write_sheet(rows)
    if drafts:
        os.makedirs(DIR, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(drafts, f, ensure_ascii=False, indent=1)
    empty = sum(1 for r in rows.values() for c in codes if not r[c] and r["en"].strip())
    print("filled %d cells from DeepL, copied %d; %d cells still empty" % (filled, copied, empty))


def collect(batch):
    """<batch>/call<n>.json (DeepL's answers as they came) -> <batch>.out.json."""
    with open(os.path.join(DIR, batch + ".json"), encoding="utf-8") as f:
        b = json.load(f)
    out, missing = {}, []
    for n, call in enumerate(b["calls"]):
        code_of = dict(zip(call.get("langs", b.get("langs")), call.get("codes", b.get("codes"))))
        path = os.path.join(DIR, batch, "call%d.json" % n)
        if not os.path.exists(path):
            missing.append(n)
            continue
        with open(path, encoding="utf-8") as f:
            answer = json.load(f)
        for per in answer["translations"]:
            code = code_of.get(per["targetLang"].upper())
            texts = [t["text"] for t in per["translations"]]
            if code is None or len(texts) != len(call["texts"]):
                sys.exit("%s: %s has %s texts for %d" % (path, per["targetLang"], len(texts), len(call["texts"])))
            for ids, t in zip(call["ids"], texts):
                for rid in ids:
                    out.setdefault(rid, {})[code] = t
    with open(os.path.join(DIR, batch + ".out.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("%s: %d rows from %d calls%s" % (batch, len(out), len(b["calls"]) - len(missing),
                                          ", missing calls %s" % missing if missing else ""))


def review(paths):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = {r["id"]: r for r in csv.DictReader(f)}
    terms = sorted(check.keep_terms(), key=len, reverse=True)
    cmap = check.font_cmap()
    done = refused = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            fixes = json.load(f)
        for rid, per in fixes.items():
            r = rows.get(rid)
            for code, fx in per.items():
                t, why = fx["text"], fx.get("why", "").strip()
                if r is None or code not in r or not r[code] or t == r[code]:
                    continue
                old = r[code]
                bad = problems(r, code, t, terms, cmap, same_lines_as=old)
                if bad:
                    print("refused %s [%s] %r -> %r: %s" % (rid, code, old, t, ", ".join(bad)))
                    refused += 1
                    continue
                r[code] = t
                r["notes"] = (r["notes"] + "; post-edit %s: %s (review)" % (code, why or "fixed")).strip("; ")
                done += 1
    extract.write_sheet(rows)
    print("applied %d fixes, refused %d" % (done, refused))


def fill(paths):
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        rows = {r["id"]: r for r in csv.DictReader(f)}
    terms = sorted(check.keep_terms(), key=len, reverse=True)
    cmap = check.font_cmap()
    by_en = {}
    for r in rows.values():
        by_en.setdefault(r["en"], []).append(r)
    done = refused = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            texts = json.load(f)
        for en, per in texts.items():
            for r in by_en.get(en, []):
                for code, t in per.items():
                    if code not in r or r[code] or not t:
                        continue
                    bad = problems(r, code, t, terms, cmap)
                    if bad:
                        print("refused %s [%s] %r -> %r: %s" % (r["id"], code, en, t, ", ".join(bad)))
                        refused += 1
                        continue
                    r[code] = t
                    set_claude_langs(r, sorted(set(claude_langs(r)) | {code}))
                    done += 1
    extract.write_sheet(rows)
    print("filled %d cells, refused %d" % (done, refused))


def main(argv):
    if argv[:1] == ["prepare"]:
        only = [argv[i + 1] for i, a in enumerate(argv) if a == "--lang"]
        prepare(only, "--claude-drafts" in argv)
    elif argv[:1] == ["merge"]:
        merge("--force" in argv)
    elif argv[:1] == ["redo-long"]:
        redo_long([argv[i + 1] for i, a in enumerate(argv) if a == "--lang"])
    elif argv[:1] == ["collect"] and len(argv) == 2:
        collect(argv[1])
    elif argv[:1] == ["review"] and len(argv) >= 2:
        review(argv[1:])
    elif argv[:1] == ["fill"] and len(argv) >= 2:
        fill(argv[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
