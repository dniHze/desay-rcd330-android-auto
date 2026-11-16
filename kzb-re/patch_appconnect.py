#!/usr/bin/env python3
"""Bake the shim's appconnect code changes into an appconnect file (the rebranded one).

    python3 -I kzb-re/patch_appconnect.py <in appconnect> <out appconnect>

The changes and their guard words are read from aa-carlife-shim/src/appc_patch.hpp, the same
tables the shim applies to the running process, so the two can never differ. A change has one
table pair per appconnect build (k<Name>, k<Name>0394, ...); the one whose guard words all match
this file is used, and none or more than one aborts. Each change is written only when its edit
words are all stock; a file that already has it is left as is. appconnect is ET_EXEC with its text mapped from file offset 0 at 0x8000, so
file offset = address - 0x8000 (checked from the ELF header). Anything unexpected aborts without
writing.

Run with python3 -I on firmware files. Never modifies its input.
"""
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "aa-carlife-shim", "src", "appc_patch.hpp")


def tables(text):
    """{name: {build: (guards [(addr, word)], edits [(addr, stock, patched)])}} from appc_patch.hpp.

    The tables without a build suffix are the 0358 build's; k<Name>0394... are the 0394 build's."""
    out = {}
    for m in re.finditer(r"const (Word|Edit) k(\w+?)(Guards|Edits)\[\] = \{(.*?)\n\};", text, re.S):
        kind, name, part, body = m.groups()
        body = re.sub(r"//[^\n]*", "", body)
        rows = [tuple(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", line))
                for line in re.findall(r"\{([^}]*)\}", body)]
        want = 2 if kind == "Word" else 3
        assert all(len(r) == want for r in rows), (name, part)
        b = re.match(r"^(\w+?)(\d{4})?$", name)
        build = b.group(2) or "0358"
        variants = out.setdefault(b.group(1).lower(), {})
        variants.setdefault(build, [None, None])[0 if part == "Guards" else 1] = rows
    assert out and all(g and e for v in out.values() for g, e in v.values()), "patch tables not found"
    return out


def text_base(elf):
    """File offset 0 maps to this address (the first PT_LOAD, R+X, offset 0)."""
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "not a 32-bit LE ELF"
    e_type, e_machine = struct.unpack_from("<HH", elf, 16)
    assert e_type == 2 and e_machine == 40, "not an ARM ET_EXEC"
    phoff, = struct.unpack_from("<I", elf, 28)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if p_type == 1 and p_offset == 0 and p_flags & 1:
            return p_vaddr, p_filesz
    sys.exit("no text segment at file offset 0")


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, dst = argv[1], argv[2]
    data = bytearray(open(src, "rb").read())
    base, size = text_base(bytes(data))
    word = lambda a: struct.unpack_from("<I", data, a - base)[0]
    inside = lambda a: base <= a < base + size - 3
    for name, variants in sorted(tables(open(HEADER).read()).items()):
        match = [b for b, (guards, edits) in sorted(variants.items())
                 if all(inside(a) for a, _ in guards) and all(inside(e[0]) for e in edits)
                 and all(word(a) == w for a, w in guards)]
        if len(match) != 1:
            sys.exit("%s: the guard words match %s of the builds %s" % (name, match or "none",
                                                                        sorted(variants)))
        guards, edits = variants[match[0]]
        state = [word(a) == p and "patched" or word(a) == s and "stock" or "other" for a, s, p in edits]
        if all(s == "patched" for s in state):
            print("%-11s %s: already in %s" % (name, match[0], src))
            continue
        if not all(s == "stock" for s in state):
            sys.exit("%s: the code to change is neither stock nor patched" % name)
        for a, s, p in edits:
            struct.pack_into("<I", data, a - base, p)
        print("%-11s %s: %d word(s) at 0x%x" % (name, match[0], len(edits), edits[0][0]))
    open(dst, "wb").write(bytes(data))
    os.chmod(dst, 0o755)
    print("in  %s %s\nout %s %s" % (hashlib.md5(open(src, "rb").read()).hexdigest(), src,
                                     hashlib.md5(bytes(data)).hexdigest(), dst))


if __name__ == "__main__":
    main(sys.argv)
