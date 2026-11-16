#!/usr/bin/env python3
"""Where each phrase of the phrase book is used: app, function, how the id was found, and the
strings the function names (kzb node paths, prefab names), for writing translator context.

    python3 -I i18n/callsites.py <base> [<base>...]     (after `i18n/fw.py unpack`)

Writes out/i18n/evidence-<base>.json: {"<id>": [{"app", "bin", "func", "how", "via", "strings"}]}.

Every app binary is disassembled with the toolchain image's objdump (docker, desay-aa:latest) and
each call to GetVWString/GetVWStringEx is resolved to its phrase id by a forward pass over the
calling function that tracks constants in registers and stack slots:
  * "const": the id is a constant at the call (mov, movw/movt, literal pool, conditional moves
    giving several ids);
  * "arg": the id is the function's own argument; callers that pass a constant there give the
    ids (two levels up), recorded with the caller in "via";
  * "ret": the id is another function's return value; the constants that function returns
    give the ids;
  * "table": the id is loaded from a table of ids in read-only data that the function points
    at (32-bit or 16-bit entries, read while they are valid ids: may read past the table).
The pass is linear (no control-flow graph): good enough for context, not for proof.

The binaries keep their full C++ symbols and are ET_EXEC; firmware is parsed, never executed.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fw  # noqa: E402

LOOKUPS = ("GetVWString(int)", "GetVWStringEx(int)")
INSN = re.compile(r"^\s+([0-9a-f]+):\t(\S+)(?:\t(.*?))?(?:\t[@;] (.*))?$")
FUNC = re.compile(r"^([0-9a-f]{8}) <(.*)>:$")
CALL = re.compile(r"^[0-9a-f]+ <(.*)>$")
REG = {"r%d" % i: "r%d" % i for i in range(13)}
REG.update({"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12", "sp": "sp", "lr": "lr", "pc": "pc"})
CONDS = ("eq", "ne", "cs", "cc", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")


def disassemble(paths):
    """{path: objdump -d -C text}, one container for all of them."""
    root = os.path.commonpath([os.path.dirname(p) for p in paths])
    script = "for f in \"$@\"; do echo \"@@@FILE $f\"; arm-linux-gnueabihf-objdump -d -C --no-show-raw-insn \"/j/$f\"; done"
    rel = [os.path.relpath(p, root) for p in paths]
    out = subprocess.run(["docker", "run", "--rm", "--platform", "linux/arm64", "-v", root + ":/j:ro", "desay-aa:latest",
                          "bash", "-c", script, "x"] + rel, check=True, stdout=subprocess.PIPE).stdout.decode("utf-8", "replace")
    res, cur = {}, None
    for line in out.split("\n"):
        if line.startswith("@@@FILE "):
            cur = os.path.join(root, line[8:])
            res[cur] = []
        elif cur:
            res[cur].append(line)
    return res


def functions(lines):
    """[(name, start, [(addr, mnemonic, operands, comment)])]."""
    out, cur = [], None
    for line in lines:
        m = FUNC.match(line)
        if m:
            cur = (m.group(2), int(m.group(1), 16), [])
            out.append(cur)
            continue
        m = INSN.match(line)
        if m and cur:
            cur[2].append((int(m.group(1), 16), m.group(2), (m.group(3) or "").strip(), (m.group(4) or "").strip()))
    return out


def split_cond(mn):
    """(base mnemonic, conditional?) for data instructions; branches are handled apart."""
    for c in CONDS:
        if mn.endswith(c) and len(mn) > len(c) and mn[:-len(c)] in ("mov", "mvn", "movw", "movt", "ldr", "add", "sub",
                                                                  "ldrh", "ldrsh", "movs", "adds", "subs"):
            return mn[:-len(c)], True
    return mn, False


def imm(s):
    s = s.strip().lstrip("#")
    try:
        return int(s, 0)
    except ValueError:
        return None


def is_text(b):
    if not b or len(b) < 3 or len(b) > 200:
        return False
    try:
        t = b.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(c.isprintable() or c == "\n" for c in t)


class Binary:
    def __init__(self, path, lines, nphrases):
        self.elf = fw.Elf(path)
        self.funcs = functions(lines)
        self.by_start = {start: name for name, start, _ in self.funcs}
        self.n = nphrases
        ro = [s for n, s in self.elf.sections.items() if n in (".rodata", ".data.rel.ro", ".data")]
        self.ro = [(s["addr"], s["addr"] + s["size"]) for s in ro]

    def in_ro(self, v):
        return any(a <= v < b for a, b in self.ro)

    def table(self, va, width):
        """Ids read from a table at va (width 4 or 2) while they are valid ids, at most 64."""
        out = []
        for i in range(64):
            o = self.elf.off(va + i * width)
            if o is None or o + width > len(self.elf.data):
                break
            v = int.from_bytes(self.elf.data[o:o + width], "little", signed=True)
            if not 0 <= v < self.n:
                break
            out.append(v)
        return out if len(out) >= 2 else []

    def scan(self, insns, lookups):
        """Forward pass over one function. Returns (sites, strings, returns, calls):
        sites [(addr, callee, value)], strings [text], returns {ints}, calls [(addr, callee, {reg: value})].
        A value is a frozenset of ints, ("arg", n), ("ret", callee), ("table", [ids]) or None."""
        regs, stack, sp = {}, {}, 0
        frame_sp = None
        sites, strings, returns, calls = [], [], set(), []
        for r in range(4):
            regs["r%d" % r] = ("arg", r)

        def val(op):
            op = op.strip()
            if op.startswith("#"):
                v = imm(op)
                return None if v is None else frozenset([v & 0xffffffff])
            return regs.get(REG.get(op))

        def setreg(rd, v, cond):
            if cond:
                old = regs.get(rd)
                if isinstance(old, frozenset) and isinstance(v, frozenset):
                    v = old | v
                elif v is None:
                    v = old
            regs[rd] = v
            if isinstance(v, frozenset) and len(v) == 1:
                p = next(iter(v))
                if self.in_ro(p):
                    s = self.elf.cstr(p)
                    if is_text(s):
                        t = s.decode("utf-8")
                        if t not in strings:
                            strings.append(t)

        for addr, mn, ops, comment in insns:
            parts = [p.strip() for p in re.split(r",(?![^\[{]*[\]}])", ops)] if ops else []
            if mn in ("bl", "blx") or (mn.startswith("bl") and mn[2:] in CONDS):
                m = CALL.match(ops)
                callee = m.group(1) if m else None
                if callee:
                    name = callee[:-4] if callee.endswith("@plt") else callee
                    if name in lookups:
                        sites.append((addr, name, regs.get("r0")))
                    args = {k: regs.get("r%d" % k) for k in range(4)}
                    args.update({k: stack.get(sp + 4 * (k - 4)) for k in range(4, 12)})
                    calls.append((addr, name, args))
                for r in ("r0", "r1", "r2", "r3", "r12", "lr"):
                    regs[r] = None
                regs["r0"] = ("ret", callee[:-4] if callee and callee.endswith("@plt") else callee)
                continue
            if mn in ("b", "bx") or (mn == "pop" and "pc" in ops) or (mn.startswith("ldm") and "pc" in ops):
                if mn != "b" or not CALL.match(ops) or True:
                    r0 = regs.get("r0")
                    if mn != "b" and isinstance(r0, frozenset):
                        returns |= r0
                regs = {}
                stack = {}
                if frame_sp is not None:
                    sp = frame_sp
                continue
            if mn.startswith("b") and mn[1:] in CONDS:
                continue
            base, cond = split_cond(mn)
            if base == "push":
                sp -= 4 * len(ops.strip("{}").split(","))
                continue
            if base == "sub" and len(parts) == 3 and parts[0] == "sp" and parts[1] == "sp":
                v = imm(parts[2])
                if v is not None:
                    sp -= v
                    frame_sp = sp
                continue
            if base in ("mov", "movs", "mvn") and len(parts) == 2:
                rd = REG.get(parts[0])
                if not rd:
                    continue
                if rd == "sp":
                    continue
                v = val(parts[1])
                if base == "mvn" and isinstance(v, frozenset):
                    v = frozenset((~x) & 0xffffffff for x in v)
                setreg(rd, v, cond)
                continue
            if base == "movw" and len(parts) == 2:
                rd = REG.get(parts[0])
                v = imm(parts[1])
                if rd:
                    setreg(rd, None if v is None else frozenset([v]), cond)
                continue
            if base == "movt" and len(parts) == 2:
                rd = REG.get(parts[0])
                v, old = imm(parts[1]), regs.get(REG.get(parts[0]))
                if rd:
                    setreg(rd, frozenset(((x & 0xffff) | (v << 16)) for x in old)
                           if v is not None and isinstance(old, frozenset) else None, cond)
                continue
            if base in ("ldr", "ldrh", "ldrsh") and len(parts) >= 2:
                rd = REG.get(parts[0])
                if not rd or rd == "pc":
                    continue
                src = ",".join(parts[1:])
                m = re.match(r"\[pc, #(-?\d+)\]", src)
                if m and base == "ldr":
                    lit = re.match(r"([0-9a-f]+)", comment)
                    w = self.elf.word(int(lit.group(1), 16)) if lit else None
                    setreg(rd, None if w is None else frozenset([w]), cond)
                    continue
                m = re.match(r"\[sp(?:, #(-?\d+))?\]$", src)
                if m:
                    slot = sp + int(m.group(1) or 0)
                    v = stack.get(slot)
                    if v is None and slot >= 0:
                        v = ("arg", 4 + slot // 4)
                    setreg(rd, v, cond)
                    continue
                m = re.match(r"\[(\w+), (\w+)(?:, lsl #(\d))?\]$", src)
                if m:
                    b, i = regs.get(REG.get(m.group(1))), regs.get(REG.get(m.group(2)))
                    for ptr in (b, i):
                        if isinstance(ptr, frozenset) and len(ptr) == 1 and self.in_ro(next(iter(ptr))):
                            ids = self.table(next(iter(ptr)), 4 if base == "ldr" else 2)
                            if ids:
                                setreg(rd, ("table", ids), cond)
                                break
                    else:
                        setreg(rd, None, cond)
                    continue
                m = re.match(r"\[(\w+), #(-?\d+)\]$", src)
                if m:
                    b = regs.get(REG.get(m.group(1)))
                    if isinstance(b, frozenset) and len(b) == 1 and self.in_ro(next(iter(b))):
                        ptr = next(iter(b)) + int(m.group(2))
                        w = self.elf.word(ptr)
                        ids = self.table(ptr, 4 if base == "ldr" else 2)
                        setreg(rd, ("table", ids) if ids else (None if w is None else frozenset([w])), cond)
                        continue
                setreg(rd, None, cond)
                continue
            if base == "str" and len(parts) == 2:
                m = re.match(r"\[sp(?:, #(-?\d+))?\]$", parts[1])
                if m:
                    stack[sp + int(m.group(1) or 0)] = regs.get(REG.get(parts[0]))
                continue
            if base == "add" and len(parts) == 4 and parts[3].startswith(("lsl", "asr", "lsr")):
                rd, x = REG.get(parts[0]), val(parts[1])
                if rd and rd not in ("sp", "pc"):
                    # base + index << n: keep the base, so a load from it reads the table there
                    setreg(rd, x if isinstance(x, frozenset) and len(x) == 1 and self.in_ro(next(iter(x))) else None, cond)
                continue
            if base in ("add", "sub") and len(parts) == 3:
                rd = REG.get(parts[0])
                if not rd or rd == "sp":
                    continue
                a = "pc" if parts[1] == "pc" else None
                if base == "add" and (parts[1] == "pc" or parts[2] == "pc"):
                    other = parts[2] if parts[1] == "pc" else parts[1]
                    v = val(other)
                    setreg(rd, frozenset(((x + addr + 8) & 0xffffffff) for x in v) if isinstance(v, frozenset) else None, cond)
                    continue
                x, y = val(parts[1]), val(parts[2]) if "lsl" not in parts[2] and "lsr" not in parts[2] else None
                if isinstance(x, frozenset) and isinstance(y, frozenset) and len(x) * len(y) <= 16:
                    sign = 1 if base == "add" else -1
                    setreg(rd, frozenset(((p + sign * q) & 0xffffffff) for p in x for q in y), cond)
                else:
                    if base == "add" and isinstance(x, frozenset) and len(x) == 1 and self.in_ro(next(iter(x))):
                        setreg(rd, x, cond)  # base of an indexed table
                    else:
                        setreg(rd, None, cond)
                continue
            if parts:
                rd = REG.get(parts[0])
                if rd and rd not in ("sp", "pc") and not mn.startswith(("str", "cmp", "cmn", "tst", "teq", "push", "vst")):
                    regs[rd] = None
        return sites, strings, returns, calls


def ids_of(v, n):
    if isinstance(v, frozenset):
        return sorted(x for x in v if 0 <= x < n)
    if isinstance(v, tuple) and v[0] == "table":
        return list(v[1])
    return []


def analyse(base):
    tree = fw.unpack(base)
    book = fw.phrasebook(os.path.join(tree, "svp", "lib", "libUIPublic.so"))
    n = len(book)
    paths = []
    for app in sorted(os.listdir(os.path.join(tree, "apps"))):
        bindir = os.path.join(tree, "apps", app, "bin")
        for b in sorted(os.listdir(bindir)):
            p = os.path.join(bindir, b)
            with open(p, "rb") as f:
                if f.read(4) == b"\x7fELF":
                    paths.append(p)
    dis = disassemble(paths)
    evidence = {}
    stats = {}

    def add(ident, rec):
        lst = evidence.setdefault(str(ident), [])
        if rec not in lst:
            lst.append(rec)

    for path in paths:
        if not any("GetVWString" in l for l in dis[path]):
            continue
        app = os.path.basename(os.path.dirname(os.path.dirname(path))).replace("sv.g5.mib2.", "")
        binary = os.path.basename(path)
        b = Binary(path, dis[path], n)
        scans = {}
        for name, start, insns in b.funcs:
            scans[name] = b.scan(insns, LOOKUPS)
        # Functions whose own argument k is the id, and functions whose return value is the id.
        argfuncs, retuses = {}, {}
        found = unresolved = 0
        for name, (sites, strings, returns, calls) in scans.items():
            for addr, callee, v in sites:
                ids = ids_of(v, n)
                how = "table" if isinstance(v, tuple) and v[0] == "table" else "const"
                if ids:
                    found += 1
                    for i in ids:
                        add(i, {"app": app, "bin": binary, "func": name, "how": how, "via": None, "strings": strings[:12]})
                elif isinstance(v, tuple) and v[0] == "arg":
                    argfuncs.setdefault(name, set()).add(v[1])
                elif isinstance(v, tuple) and v[0] == "ret" and v[1]:
                    retuses.setdefault(v[1], set()).add(name)
                else:
                    unresolved += 1
                    add("?", {"app": app, "bin": binary, "func": name, "how": "unresolved", "via": None,
                              "strings": strings[:12]})
        # Two levels of "the id is my argument": callers passing constants.
        level = dict(argfuncs)
        for depth in range(2):
            nxt = {}
            for caller, (sites, strings, returns, calls) in scans.items():
                for addr, callee, args in calls:
                    if callee not in level:
                        continue
                    for k in level[callee]:
                        v = args.get(k)
                        ids = ids_of(v, n)
                        if ids:
                            found += 1
                            for i in ids:
                                add(i, {"app": app, "bin": binary, "func": callee, "how": "arg", "via": caller,
                                        "strings": strings[:12]})
                        elif isinstance(v, tuple) and v[0] == "arg":
                            nxt.setdefault(caller, set()).add(v[1])
            level = nxt
        for callee, users in retuses.items():
            if callee in scans:
                for i in sorted(x for x in scans[callee][2] if 0 <= x < n):
                    for user in sorted(users):
                        add(i, {"app": app, "bin": binary, "func": user, "how": "ret", "via": callee,
                                "strings": scans[user][1][:12]})
        stats[app + "/" + binary] = (found, unresolved)
    out = os.path.join(fw.OUT, "evidence-%s.json" % base)
    with open(out, "w") as f:
        json.dump({"base": base, "phrases": n, "evidence": evidence}, f, ensure_ascii=False, indent=0)
    reached = len([k for k in evidence if k != "?"])
    print("%s: %d phrases, %d reached from code; %s -> %s" % (base, n, reached,
          ", ".join("%s %d/%d" % (k, a, u) for k, (a, u) in sorted(stats.items())), os.path.relpath(out, fw.WORK)))


def main(argv):
    for base in argv or fw.BASE_NAMES:
        analyse(base)


if __name__ == "__main__":
    main(sys.argv[1:])
