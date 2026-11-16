#!/usr/bin/env python3
"""The apps' own texts: texts an app holds itself, outside the phrase book, and where its code
reads them.

    python3 -I i18n/literals.py list <base>              candidate texts of the UI functions, by app
    python3 -I i18n/literals.py check <base>             which "lit:" rows of the sheet this base has
    python3 -I i18n/literals.py table <base> <out .inc>  the redirect table for our library
    python3 -I i18n/literals.py verify <.inc> <app> <binary>  the table's words hold in that binary

Some apps pick a text of their own instead of a phrase: AppConnect's pop-ups and screens
("Welcome to App-Connect.", the MirrorLink and connection pop-ups), carmenu's service-interval
pieces ("in ", " or ", " days"), "SD card" in the launcher and the media player, a pop-up in
toptip. The code reaches each English text through a literal-pool word holding its offset from
the instruction that adds pc ("ldr rX, [pc, #n]" then "add rX, pc, rX", either possibly
conditional; the Chinese one is picked when the car's code is 1). Our library rewrites those
words in the app's private copy of its code, so the same instruction reads the chosen language's
text (i18n/lib/literals.cpp); Chinese texts are never touched.

The sheet's "lit:<app>:<n>" rows hold them, one row per message: a pop-up's lines joined by \\n,
each line being one text of the app (a line with %1 is filled in by the app and stays as it is).
Some messages are also one text with real line breaks elsewhere in the app; a row covers both.
The entries (entries()): per row, the whole text, then each line when it has several. The
library and aa_lang.bin number them alike and both carry the md5 of their English (lit_md5).

table: for every entry, every literal-pool word of the app's UI functions (UI below) that
reaches that text: {pool word address, stock word, add address, add instruction, entry}, per
app, with the GOT slot of SVP_GetLanguage (the library refreshes the texts there, before the app
picks them; none if the slot is under RELRO). Every row must be found in the base (check lists
what a base lacks). verify: every word and add instruction of one app's sites is as the table
says in another build of that app (the Android Auto AppConnect: rebranded texts and code
changes). Generated at build time from the firmware; never committed.

Firmware is parsed, never executed (objdump in the desay-aa:latest container).
"""
import csv
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import callsites  # noqa: E402
import fw  # noqa: E402

# Per app: the functions whose own texts are shown (a regular expression on the C++ name). Only
# their literal-pool words are ever rewritten.
UI = {
    "appconnect": r"PopUpMgr::|UpdateLanguageChange|UpdateTipLanguage|UpdateVNC\(",
    "carmenu": r"^CarSetupServiceIntervalDlg::",
    "launcher": r"^SVPSourceMgr::GetScrString",
    "mediaplayer": r"^ShowFaultSdcardPop::UpdateTipText",
    "toptip": r"^CarFPASetupDlg::InitResetPopup",
}
GET_LANGUAGE = "_Z15SVP_GetLanguageRj"
CJK = re.compile("[一-鿿]")
CONDS = "(eq|ne|cs|cc|hs|lo|mi|pl|hi|ls|ge|lt|gt|le)?"
INSN = re.compile(r"^\s+([0-9a-f]+):\t(\S+)\t(.*?)(?:\t@ ([0-9a-f]+).*)?$")
NO_DEST = re.compile(r"^(str|cmp|cmn|tst|teq|b|push|stm|vst|vcmp|nop|svc|dmb|pld)")
CALL = re.compile(r"^bl")
SCRATCH = ("r0", "r1", "r2", "r3", "ip", "lr")
NEAR = 64  # an add pc must follow its literal load within this many bytes
# A call that takes the text with its length, (const char*, size_t): std::string's assign, append
# and constructor. Its length is an immediate next to the text's load ("mov r2, #19"), the English
# text's byte length; a longer translation must change it too, or the call copies the English
# length of it (step 4 run on 0317: Ukrainian pop-ups cut mid-letter).
LENGTH_CALL = re.compile(r"\(char const\*, unsigned int[,)]")


def binary(base, app):
    return os.path.join(fw.tree(base), "apps", "sv.g5.mib2." + app, "bin", app)


def sites(e, lines):
    """[(function, pool word address, add address, string address)] of every "ldr rX, [pc, #n]"
    + "add rX, pc, rX" pair, and the pool words loaded but never added to pc (absolute)."""
    out, absolute, fn, lit = [], [], None, {}

    def drop(reg):
        if reg in lit:
            _, pool, at = lit.pop(reg)
            absolute.append((fn, pool, at))
    for line in lines:
        m = callsites.FUNC.match(line)
        if m:
            for reg in list(lit):
                drop(reg)
            fn = m.group(2)
            continue
        m = INSN.match(line)
        if not m:
            continue
        addr, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
        r = re.match(r"(\w+), \[pc, #-?\d+\]$", ops)
        if re.match("ldr" + CONDS + "$", mn) and r and m.group(4):
            drop(r.group(1))
            pool = int(m.group(4), 16)
            lit[r.group(1)] = (e.word(pool), pool, addr)
            continue
        r = re.match(r"(\w+), pc, (\w+)$", ops)
        if re.match("add" + CONDS + "$", mn) and r and r.group(2) in lit and r.group(1) == r.group(2):
            word, pool, at = lit.pop(r.group(2))
            if addr - at <= NEAR:
                out.append((fn, pool, addr, (word + addr + 8) & 0xFFFFFFFF))
            continue
        if CALL.match(mn):
            for reg in SCRATCH:
                drop(reg)
        elif mn.startswith(("pop", "ldm")):
            for reg in re.findall(r"\b(r\d+|ip|lr|sl|fp|sb)\b", ops):
                drop(reg)
        elif not NO_DEST.match(mn):
            drop(ops.split(",")[0].strip())
    for reg in list(lit):
        drop(reg)
    return out, absolute


def text_at(e, va):
    ro = e.sections[".rodata"]
    if not ro["addr"] <= va < ro["addr"] + ro["size"]:
        return None
    return e.cstr(va)


def relro(e):
    """(start, end) of the PT_GNU_RELRO range, or None."""
    phoff = struct.unpack_from("<I", e.data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", e.data, 42)
    for i in range(phnum):
        ptype, _, vaddr, _, _, memsz, _, _ = struct.unpack_from("<8I", e.data, phoff + i * phentsize)
        if ptype == 0x6474E552:
            return vaddr, vaddr + memsz
    return None


def got_slot(e, name):
    """The GOT slot address of an imported function (its R_ARM_JUMP_SLOT), or None (also when it is
    read-only after start, under RELRO)."""
    rel, dyn, st = e.sections.get(".rel.plt"), e.sections[".dynsym"], e.sections[".dynstr"]
    ro = relro(e)
    if not rel:
        return None
    for k in range(rel["size"] // 8):
        offset, info = struct.unpack_from("<II", e.data, rel["off"] + 8 * k)
        if info & 0xFF != 22:  # R_ARM_JUMP_SLOT
            continue
        sym = info >> 8
        sname = struct.unpack_from("<I", e.data, dyn["off"] + 16 * sym)[0]
        if e.cstr_at(st["off"] + sname).decode("latin1") == name:
            return None if ro and ro[0] <= offset < ro[1] else offset
    return None


def calls_of(lines):
    """{address: callee as objdump names it} of every bl."""
    out = {}
    for line in lines:
        m = INSN.match(line)
        if m and CALL.match(m.group(2)):
            c = re.search(r"<(.+)>", m.group(3))
            out[int(m.group(1), 16)] = c.group(1) if c else ""
    return out


def mov_imm(insn):
    """(condition, Rd, value) of "mov{cond} Rd, #imm" (ARM, no S), or None."""
    if insn & 0x0FFF0000 != 0x03A00000:
        return None
    rot, imm8 = (insn >> 8) & 15, insn & 0xFF
    value = ((imm8 >> (2 * rot)) | (imm8 << (32 - 2 * rot))) & 0xFFFFFFFF if rot else imm8
    return insn >> 28, (insn >> 12) & 15, value


def length_site(e, calls, add, n):
    """(address, instruction) of the "mov r2, #n" that gives the call after `add`, one taking
    (const char*, size_t), the text's byte length n; (0, 0) when the text goes alone."""
    call = next((a for a in range(add + 4, add + 20, 4) if a in calls), None)
    if call is None or not LENGTH_CALL.search(calls[call]):
        return 0, 0
    for a in list(range(add - 12, add, 4)) + list(range(add + 4, call, 4)):
        if a in calls:
            continue
        m = mov_imm(e.word(a))
        if m and m[0] != 15 and m[1] == 2 and m[2] == n:
            return a, e.word(a)
    return 0, 0


def ui_sites(base, apps):
    """{app: (Elf, [(function, pool, add, string address, text bytes)], [absolute loads], calls)}
    over the UI functions of the apps this base has."""
    fw.unpack(base)
    paths = {app: binary(base, app) for app in apps if os.path.exists(binary(base, app))}
    dis = callsites.disassemble(list(paths.values()))
    out = {}
    for app, path in paths.items():
        e = fw.Elf(path)
        ss, absolute = sites(e, dis[path])
        keep = [(fn, pool, add, va, text_at(e, va)) for fn, pool, add, va in ss if re.search(UI[app], fn or "")]
        out[app] = (e, [s for s in keep if s[4]], [a for a in absolute if re.search(UI[app], a[0] or "")],
                    calls_of(dis[path]))
    return out


# ---- the sheet's rows ----

def lit_rows():
    import extract
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        return [r for r in csv.DictReader(f) if r["id"].startswith("lit:")]


def app_of(rid):
    return rid.split(":")[1]


def dynamic(line):
    return "%1" in line


def entries(rows):
    """[(row id, line index or -1 for the whole text, English bytes)] in the sheet's order."""
    out = []
    for r in rows:
        lines = r["en"].split("\\n")
        out.append((r["id"], -1, "\n".join(lines).encode("utf-8")))
        if len(lines) > 1:
            out.extend((r["id"], j, l.encode("utf-8")) for j, l in enumerate(lines))
    return out


def lit_md5(ents):
    return hashlib.md5(b"".join(t + b"\0" for _, _, t in ents)).hexdigest()


def resolve(base, rows=None):
    """(sites, covered row ids, problems, apps) for one base. sites: per app, [(pool, word, add,
    insn, entry index, length address, length instruction)]; apps: {app: (GOT slot, .plt address)}."""
    rows = lit_rows() if rows is None else rows
    ents = entries(rows)
    found = ui_sites(base, sorted({app_of(r["id"]) for r in rows}))
    out, covered, problems, apps = {}, set(), [], {}
    for app, (e, ss, absolute, calls) in found.items():
        by_text = {}
        for fn, pool, add, va, t in ss:
            by_text.setdefault(t, []).append((fn, pool, add, va))
        mine = [(k, rid, j, t) for k, (rid, j, t) in enumerate(ents) if app_of(rid) == app]
        seen = {}
        for k, rid, j, t in mine:
            if j >= 0 and dynamic(t.decode()):
                continue
            if t in seen and seen[t][0] != rid:
                problems.append("%s: %r is in rows %s and %s" % (app, t.decode(), seen[t][0], rid))
            seen[t] = (rid, k)
        used, targets = [], set()
        for r in rows:
            if app_of(r["id"]) != app:
                continue
            ks = [(k, j, t) for k, rid, j, t in mine if rid == r["id"]]
            whole = [k for k, j, t in ks if j < 0 and by_text.get(t)]
            lines = [(k, t) for k, j, t in ks if j >= 0 and not dynamic(t.decode())]
            have = [k for k, t in lines if by_text.get(t)]
            if whole or (lines and len(have) == len(lines)):
                covered.add(r["id"])
            for k in whole + have:
                for fn, pool, add, va in by_text[ents[k][2]]:
                    used.append((pool, e.word(pool), add, e.word(add), k) + length_site(e, calls, add, len(ents[k][2])))
                    targets.add(va)
        for fn, pool, at in absolute:
            if e.word(pool) in targets:
                problems.append("%s: %s loads the text at 0x%x by its absolute address (not supported)"
                                % (app, fn, e.word(pool)))
        if len({u[0] for u in used}) != len(used):
            problems.append("%s: a literal-pool word reaches two texts" % app)
        for pool, word, add, insn, k, _, _ in used:  # add{cond} rd, pc, rd: what the library assumes
            if insn & 0x0FFF0FF0 != 0x008F0000 or (insn >> 12) & 15 != insn & 15 or insn >> 28 == 15:
                problems.append("%s: 0x%x is not add rX, pc, rX (0x%08x)" % (app, add, insn))
        out[app] = sorted(used)
        apps[app] = (got_slot(e, GET_LANGUAGE), e.sections[".plt"]["addr"])
    return out, covered, problems, apps


def coverage(base):
    return resolve(base)[1]


def table(base, out):
    rows = lit_rows()
    ents = entries(rows)
    found, covered, problems, apps = resolve(base, rows)
    missing = [r["id"] for r in rows if r["id"] not in covered and app_of(r["id"]) in found]
    if problems or missing:
        sys.exit("%s: %s" % (base, "; ".join(problems + ["not found: %s" % ", ".join(missing)] if missing else problems)))
    lines = ["// Generated by i18n/literals.py for base %s. Not committed." % base,
             "#define AA_LIT_COUNT %d" % len(ents),
             '#define AA_LIT_MD5 "%s"' % lit_md5(ents),
             "static const LitSite kLitSites[] = {"]
    heads, n = [], 0
    for app in sorted(found):
        heads.append('    {"%s", 0x%x, 0x%x, %d, %d},' % (app, apps[app][0] or 0, apps[app][1], n, len(found[app])))
        for pool, word, add, insn, k, at, mov in found[app]:
            lines.append("    {0x%x, 0x%08x, 0x%x, 0x%08x, %d, 0x%x, 0x%08x}," % (pool, word, add, insn, k, at, mov))
            n += 1
    lines += ["};", "static const LitApp kLitApps[] = {"] + heads + ["};"]
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d texts (%d rows), %d literal words in %s; %d with a length" % (
        os.path.relpath(out, fw.WORK) if out.startswith(fw.WORK) else out, len(ents), len(rows), n,
        ", ".join("%s %d" % (a, len(found[a])) for a in sorted(found)), sum(1 for a in found for u in found[a] if u[5])))


def verify(inc, app, path):
    """Exits if a word or add instruction of `app`'s sites in the table differs in `path`."""
    text = open(inc).read()
    m = re.search(r'\{"%s", 0x[0-9a-f]+, 0x[0-9a-f]+, (\d+), (\d+)\}' % app, text)
    if not m:
        sys.exit("%s: no sites of %s" % (inc, app))
    sites = re.findall(r"\{0x([0-9a-f]+), 0x([0-9a-f]+), 0x([0-9a-f]+), 0x([0-9a-f]+), \d+, 0x([0-9a-f]+), 0x([0-9a-f]+)\}", text)
    first, count = int(m.group(1)), int(m.group(2))
    e = fw.Elf(path)
    bad = [s for s in sites[first:first + count]
           if e.word(int(s[0], 16)) != int(s[1], 16) or e.word(int(s[2], 16)) != int(s[3], 16)
           or (int(s[4], 16) and e.word(int(s[4], 16)) != int(s[5], 16))]
    if bad:
        sys.exit("%s: %d of %d sites of %s differ, e.g. pool 0x%s" % (path, len(bad), count, app, bad[0][0]))
    print("%s: the %d sites of %s hold" % (os.path.basename(path), count, app))


def candidates(base):
    """{app: [(text, [functions])]}: the visible-looking texts of the UI functions, for finding new
    rows (log formats, paths, node names and Chinese left out)."""
    out = {}
    for app, (e, ss, _, _) in ui_sites(base, sorted(UI)).items():
        found = {}
        for fn, _, _, va, t in ss:
            try:
                s = t.decode("utf-8")
            except UnicodeDecodeError:
                continue
            if CJK.search(s) or not re.search(r"[A-Za-z]", s) or "(%u|" in s or "/media/" in s:
                continue
            if re.fullmatch(r"[\w|/:.&-]+", s) and not re.fullmatch(r"[A-Z]?[a-z]+", s):
                continue
            found.setdefault((va, s), []).append(fn)
        out[app] = [(s, sorted(set(fns))) for (va, s), fns in sorted(found.items())]
    return out


def main(argv):
    if argv[:1] == ["list"] and len(argv) == 2:
        for app, items in candidates(argv[1]).items():
            print("== %s: %d" % (app, len(items)))
            for t, fns in items:
                print("   %-60s %s" % (t.replace("\n", "\\n")[:60], ", ".join(f.split("(")[0] for f in fns)[:80]))
    elif argv[:1] == ["check"] and len(argv) == 2:
        rows = lit_rows()
        found, covered, problems, apps = resolve(argv[1], rows)
        for p in problems:
            print("problem: " + p)
        for r in rows:
            if r["id"] not in covered:
                print("not found: %s %r" % (r["id"], r["en"]))
        print("%s: %d of %d rows found; %s" % (argv[1], len(covered), len(rows),
                                              ", ".join("%s %d words, GetLanguage slot %s" % (a, len(found[a]),
                                                        hex(apps[a][0]) if apps[a][0] else "none") for a in sorted(found))))
    elif argv[:1] == ["table"] and len(argv) == 3:
        table(argv[1], argv[2])
    elif argv[:1] == ["verify"] and len(argv) == 4:
        verify(argv[1], argv[2], argv[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
