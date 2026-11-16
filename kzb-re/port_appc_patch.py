#!/usr/bin/env python3
"""Port the appconnect code changes (aa-carlife-shim/src/appc_patch.hpp) to another appconnect build.

    python3 -I kzb-re/port_appc_patch.py <appconnect the tables are for> <other appconnect> <suffix>

Every guard and edit address in the tables is moved by its offset inside the same named function
(from each file's function labels, read with llvm-objdump, which also names the PLT entries); a
function holding one must be the same length in both. A call or branch to a function's first
instruction maps by the function's name alone (its body may differ). The instruction there must be the same once
branch targets are mapped the same way, and literal-pool loads compare equal. Patched branch words
are re-encoded for their new address and target. The output is a C++ block of tables for the
other build, named k<Patch><suffix>Guards/Edits. Anything that does not map exactly aborts: a
function missing, a body of another length, an instruction that differs.

Run with python3 -I on firmware files. Reads only; prints the tables.
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "aa-carlife-shim", "src", "appc_patch.hpp")


def tables(text):
    """[(name, guards [(addr, word, comment)], edits [(addr, stock, patched, comment)])] in file order."""
    out = {}
    order = []
    for m in re.finditer(r"const (Word|Edit) k(\w+?)(Guards|Edits)\[\] = \{(.*?)\n\};", text, re.S):
        kind, name, part, body = m.groups()
        rows = []
        for line in body.splitlines():
            r = re.match(r"\s*\{([^}]*)\},?\s*(//.*)?$", line)
            if not r:
                continue
            nums = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", r.group(1))]
            rows.append(tuple(nums) + ((r.group(2) or "").strip(),))
        if name not in out:
            out[name] = [None, None]
            order.append(name)
        out[name][0 if part == "Guards" else 1] = rows
    return [(n, out[n][0], out[n][1]) for n in order]


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        assert d[:4] == b"\x7fELF" and d[4] == 1, "not ELF32"
        phoff, = struct.unpack_from("<I", d, 28)
        phentsize, phnum = struct.unpack_from("<HH", d, 42)
        self.loads = []
        for i in range(phnum):
            t, off, va, _, fs, _, fl, _ = struct.unpack_from("<8I", d, phoff + i * phentsize)
            if t == 1:
                self.loads.append((va, off, fs))
        # Function labels from the disassembly: it names the PLT entries (printf@plt, ...) too,
        # which llvm-nm does not.
        cmd = ["llvm-objdump", "-d", "--triple=armv7-linux-gnueabihf", path]
        dis = subprocess.run(["xcrun"] + cmd, capture_output=True, text=True)
        if dis.returncode:
            dis = subprocess.run(cmd, capture_output=True, text=True)
        self.funcs = []
        for line in dis.stdout.splitlines():
            m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
            if m:
                self.funcs.append((int(m.group(1), 16), m.group(2)))
        self.funcs.sort()
        self.by_name = {}
        for a, n in self.funcs:
            self.by_name.setdefault(n, a)

    def word(self, va):
        for base, off, fs in self.loads:
            if base <= va < base + fs:
                return struct.unpack_from("<I", self.data, off + va - base)[0]
        raise KeyError(hex(va))

    def func_at(self, va):
        """(name, start, end) of the function holding va: up to the next symbol."""
        lo = None
        for i, (a, n) in enumerate(self.funcs):
            if a > va:
                break
            lo = i
        a, n = self.funcs[lo]
        end = next((b for b, _ in self.funcs[lo + 1:] if b > a), a + 0x10000)
        return n, a, end


def is_branch(w):
    return (w >> 25) & 7 == 5  # B / BL (any condition)


def branch_target(addr, w):
    imm = w & 0xFFFFFF
    if imm & 0x800000:
        imm -= 1 << 24
    return addr + 8 + (imm << 2)


def encode_branch(addr, w, target):
    off = (target - addr - 8) >> 2
    assert -(1 << 23) <= off < (1 << 23)
    return (w & 0xFF000000) | (off & 0xFFFFFF)


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    old, new, suffix = Elf(argv[1]), Elf(argv[2]), argv[3]

    def map_addr(va):
        name, start, end = old.func_at(va)
        if name not in new.by_name:
            sys.exit("function %s (0x%x) is not in the other build" % (name, start))
        nstart = new.by_name[name]
        if va == start:
            return nstart, name  # a call to the function's entry: its body may differ
        nname, _, nend = new.func_at(nstart)
        if nend - nstart != end - start:
            sys.exit("function %s: %d bytes here, %d there" % (name, end - start, nend - nstart))
        return nstart + (va - start), name

    def same_insn(va, nva):
        w, nw = old.word(va), new.word(nva)
        if is_branch(w) and is_branch(nw) and (w & 0xFF000000) == (nw & 0xFF000000):
            t, _ = map_addr(branch_target(va, w))
            if t != branch_target(nva, nw):
                # A call into the PLT or another library moves on its own; accept when both land
                # on a symbol of the same name.
                tn = old.func_at(branch_target(va, w))[0]
                ntn = new.func_at(branch_target(nva, nw))[0]
                return tn == ntn
            return True
        return w == nw

    out = []
    for name, guards, edits in tables(open(HEADER).read()):
        if re.search(r"\d{4}$", name):
            continue  # another build's tables (k<Name>0394...): only the 0358 ones are the source
        out.append("// --- %s (ported from the 0358 tables by kzb-re/port_appc_patch.py) ---" % name.lower())
        out.append("const Word k%s%sGuards[] = {" % (name, suffix))
        for addr, word, comment in guards:
            nva, fn = map_addr(addr)
            if old.word(addr) != word:
                sys.exit("%s: guard 0x%x is not 0x%08x in %s" % (name, addr, word, argv[1]))
            if not same_insn(addr, nva):
                sys.exit("%s: 0x%x (%s) and 0x%x differ: 0x%08x / 0x%08x" % (name, addr, fn, nva, old.word(addr), new.word(nva)))
            out.append("    {0x%x, 0x%08x}, %s" % (nva, new.word(nva), comment))
        out.append("};")
        out.append("const Edit k%s%sEdits[] = {" % (name, suffix))
        for addr, stock, patched, comment in edits:
            nva, fn = map_addr(addr)
            if old.word(addr) != stock:
                sys.exit("%s: edit 0x%x is not stock 0x%08x" % (name, addr, stock))
            if not same_insn(addr, nva):
                sys.exit("%s: edit 0x%x (%s) and 0x%x differ" % (name, addr, fn, nva))
            npatched = patched
            if is_branch(patched):
                t, _ = map_addr(branch_target(addr, patched))
                npatched = encode_branch(nva, patched, t)
            out.append("    {0x%x, 0x%08x, 0x%08x}, %s" % (nva, new.word(nva), npatched, comment))
        out.append("};")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv)
