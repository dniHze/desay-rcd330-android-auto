#!/usr/bin/env python3
"""The other languages' texts for the unit: i18n/strings.csv -> aa_lang.bin for one phrase book.

    python3 -I i18n/build_table.py <stock libUIPublic.so> <out strings.bin>

The layout (little-endian), read by i18n/lib/uipublic.cpp:
    0  "AALANGS\\0"
    8  u32 version (2)
   12  u32 N, the phrase count of the library it is for
   16  u32 number of languages
   20  the md5 (32 hex characters) of that library's English texts, each followed by a NUL
   52  u32 offset of the apps' own texts' header
   56  per language, in the Settings list's order (languages.csv): char code[8], u32 offset of
       its own name, u32 offset of its flag picture's path (0: none), u32 offset of its table
       (for English and Chinese, which come from the library itself: 0, or a table holding only
       the phrases i18n/stock_overrides.csv changes), u32 offset of its table of the apps' own
       texts (0 for English and Chinese)
       then the apps' own texts' header: "AALITS\0\0", u32 M (literals.entries: per "lit:" row
       the whole text, then each line when it has several), the md5 of their English
       (literals.lit_md5, 32 hex characters), u32 0
       then each table: N u32 offsets of the phrase's text, 0 where the sheet has none (the
       unit shows English there); then M u32 offsets of the own texts, alike
       then the texts, UTF-8, each ending in a NUL; the file ends in a NUL.
Line breaks written \\n in the sheet become real ones; an own text's line is that line of the
row's translation, which must have the English's number of lines.
"""
import csv
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
import fw  # noqa: E402
import gen_table  # noqa: E402
import literals  # noqa: E402

FLAGS = "/svp/share/aa_lang_%s.png"
OVERRIDES = os.path.join(HERE, "stock_overrides.csv")


def overrides():
    """{code: {phrase id: text}}: the stock English and Chinese texts the user changed."""
    out = {}
    with open(OVERRIDES, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if r["code"] not in ("en", "zh"):
                sys.exit("%s: %s is not a stock language" % (OVERRIDES, r["code"]))
            out.setdefault(r["code"], {})[int(r["id"])] = r["text"].replace("\\n", "\n")
    return out


def build(lib, out):
    book = fw.phrasebook(lib)
    n = len(book)
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        sheet = list(csv.DictReader(f))
    rows = {r["id"]: r for r in sheet if r["id"].isdigit()}
    lits = [r for r in sheet if r["id"].startswith("lit:")]
    ents = literals.entries(lits)
    lit_by_id = {r["id"]: r for r in lits}
    for i, _, en in book:
        r = rows.get(str(i))
        if r is None or r["en"] != en.decode("utf-8").replace("\n", "\\n"):
            sys.exit("phrase %d: the sheet's English differs from this library's (run i18n/extract.py)" % i)
    langs = sorted(extract.languages(), key=lambda l: int(l["order"]))
    blob = bytearray()
    where = {}

    def text(s):
        b = s.encode("utf-8") + b"\0"
        if b not in where:
            where[b] = len(blob)
            blob.extend(b)
        return where[b]

    def own(code, rid, j):
        r = lit_by_id[rid]
        t = r[code]
        if not t:
            return None
        lines, en = t.split("\\n"), r["en"].split("\\n")
        if len(lines) != len(en):
            sys.exit("%s %s: %d lines, the English has %d" % (rid, code, len(lines), len(en)))
        if j < 0:
            return text("\n".join(lines))
        return None if literals.dynamic(en[j]) else text(lines[j])

    head = 56 + 24 * len(langs)
    lit_head = head
    head += 48
    tables, own_tables = [], []
    stock = overrides()
    for l in langs:
        if l["code"] in ("en", "zh"):
            over = stock.get(l["code"], {})
            if any(i >= n for i in over):
                sys.exit("%s: phrase %s is not in this phrase book" % (OVERRIDES, max(over)))
            tables.append([text(over[i]) if i in over else None for i in range(n)] if over else None)
            own_tables.append(None)
            continue
        tables.append([text(rows[str(i)][l["code"]].replace("\\n", "\n")) if rows[str(i)][l["code"]] else None
                       for i in range(n)])
        own_tables.append([own(l["code"], rid, j) for rid, j, _ in ents])
    table_at, own_at, pos = [], [], head
    for t, o in zip(tables, own_tables):
        table_at.append(pos if t is not None else 0)
        pos += 4 * n if t is not None else 0
        own_at.append(pos if o is not None else 0)
        pos += 4 * len(ents) if o is not None else 0
    texts_at = pos
    names = [text(l["endonym"]) for l in langs]
    flags = [text(FLAGS % l["flag"]) if l["flag"] else None for l in langs]
    data = bytearray()
    data += b"AALANGS\0" + struct.pack("<III", 2, n, len(langs)) + gen_table.en_md5(book).encode() + struct.pack("<I", lit_head)
    for k, l in enumerate(langs):
        code = l["code"].encode()
        data += code + b"\0" * (8 - len(code))
        data += struct.pack("<IIII", texts_at + names[k], texts_at + flags[k] if flags[k] is not None else 0,
                            table_at[k], own_at[k])
    data += b"AALITS\0\0" + struct.pack("<I", len(ents)) + literals.lit_md5(ents).encode() + struct.pack("<I", 0)
    assert len(data) == head
    for t, o in zip(tables, own_tables):
        for part in (t, o):
            if part is not None:
                data += struct.pack("<%dI" % len(part), *[texts_at + x if x is not None else 0 for x in part])
    assert len(data) == texts_at
    data += blob
    with open(out, "wb") as f:
        f.write(data)
    have = {l["code"]: sum(1 for x in t if x is not None) for l, t in zip(langs, tables) if t is not None}
    print("%s: %d phrases, %s; %d own texts; %d bytes, md5 %s" % (out, n, ", ".join("%s %d" % kv for kv in have.items()),
                                                                  len(ents), len(data), hashlib.md5(bytes(data)).hexdigest()))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
