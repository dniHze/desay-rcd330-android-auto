#!/usr/bin/env python3
"""The stock firmware as the i18n tools read it: the unpacked image trees, a small ELF32
reader, and the phrase book in libUIPublic.so.

    python3 -I i18n/fw.py unpack [--base 0317|0394]...   unpack the stock images (default: both)
    python3 -I i18n/fw.py dump <libUIPublic.so>           print the phrase book as id, zh, en

Each base's stock container (sysupdate/build_update.py BASES: file, folder, md5s) is unpacked
into out/i18n/fw<base>/: svp/ (the /svp tree) and apps/<package>/ (each app package as
/storage/app/<package> holds it). The md5 of the container and of svp.bin and app-pkg.bin are
checked first; a tree whose marker names the same container md5 is reused.

The phrase book: libUIPublic.so exports s_Items, N entries of {int id, const char** row}, each
row a {zh, en} pair of pointers into the library's read-only data. The id equals the index.
The pointers are R_ARM_RELATIVE relocations whose addend is stored in place, so the stored word
is the address.

Firmware is parsed only, never executed. Run with python3 -I.
"""
import gzip
import hashlib
import io
import os
import struct
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)
OUT = os.path.join(WORK, "out", "i18n")
BASE_NAMES = ("0317", "0394")


def bases():
    """BASES from sysupdate/build_update.py: the one place the stock images are pinned."""
    sys.path.insert(0, os.path.join(WORK, "sysupdate"))
    try:
        import build_update
    finally:
        sys.path.pop(0)
    return build_update.BASES


def present():
    """The bases whose stock image is in place. Both have the same phrase book (libUIPublic.so),
    so the sheet's stock texts can come from one."""
    return [b for b, v in bases().items() if b in BASE_NAMES and os.path.exists(os.path.join(v["dir"], v["name"]))]


def md5(data):
    return hashlib.md5(data).hexdigest()


def tree(base):
    return os.path.join(OUT, "fw" + base)


def unpack(base):
    """Unpacks the base's stock image into out/i18n/fw<base>/ (once) and returns that folder."""
    b = bases()[base]
    src = os.path.join(b["dir"], b["name"])
    dst = tree(base)
    marker = os.path.join(dst, ".container")
    if os.path.exists(marker) and open(marker).read().strip() == b["stock"]["container"]:
        return dst
    with open(src, "rb") as f:
        if md5(f.read()) != b["stock"]["container"]:
            sys.exit("%s is not the stock %s container" % (src, base))
    partdir = os.path.join(dst, "parts")
    subprocess.run([sys.executable, "-I", os.path.join(WORK, "tools", "desay_pkg.py"), "unpack", src, partdir],
                   check=True, stdout=subprocess.DEVNULL)
    parts = {}
    for p in ("svp.bin", "app-pkg.bin"):
        with open(os.path.join(partdir, p), "rb") as f:
            parts[p] = f.read()
        if md5(parts[p]) != b["stock"][p]:
            sys.exit("stock %s %s md5 differs" % (base, p))
    for p in os.listdir(partdir):
        os.remove(os.path.join(partdir, p))
    os.rmdir(partdir)
    with tarfile.open(fileobj=io.BytesIO(parts["svp.bin"]), mode="r:gz") as t:
        t.extractall(os.path.join(dst, "svp"), filter="data")
    with tarfile.open(fileobj=io.BytesIO(parts["app-pkg.bin"]), mode="r:gz") as t:
        for m in t:
            if m.isreg() and m.name.endswith(".pkg"):
                name = os.path.basename(m.name)[:-len(".pkg")].rsplit("-", 1)[0]
                with tarfile.open(fileobj=io.BytesIO(t.extractfile(m).read()), mode="r:gz") as p:
                    p.extractall(os.path.join(dst, "apps", name), filter="data")
    with open(marker, "w") as f:
        f.write(b["stock"]["container"] + "\n")
    return dst


class Elf:
    """Just enough ELF32 little-endian ARM: sections, load segments, dynamic symbols."""

    def __init__(self, path):
        self.path = path
        self.data = b = open(path, "rb").read()
        if b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 1:
            raise ValueError("%s: not ELF32 little-endian" % path)
        self.type, self.machine = struct.unpack_from("<HH", b, 16)
        phoff, shoff = struct.unpack_from("<II", b, 28)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", b, 42)
        self.segments = []
        for i in range(phnum):
            ptype, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<8I", b, phoff + i * phentsize)
            if ptype == 1:
                self.segments.append((vaddr, off, filesz, memsz, flags))
        raw = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx][4] if shnum else 0
        self.sections = {}
        for s in raw:
            name = self.cstr_at(strtab + s[0]).decode("latin1")
            self.sections[name] = {"type": s[1], "flags": s[2], "addr": s[3], "off": s[4], "size": s[5],
                                   "link": s[6], "entsize": s[9]}

    def cstr_at(self, off):
        return self.data[off:self.data.index(b"\0", off)]

    def off(self, vaddr):
        """File offset of a virtual address, or None outside the file-backed segments."""
        for va, off, filesz, _, _ in self.segments:
            if va <= vaddr < va + filesz:
                return vaddr - va + off
        return None

    def word(self, vaddr):
        o = self.off(vaddr)
        return None if o is None or o + 4 > len(self.data) else struct.unpack_from("<I", self.data, o)[0]

    def cstr(self, vaddr):
        o = self.off(vaddr)
        if o is None:
            return None
        end = self.data.find(b"\0", o)
        return None if end < 0 else self.data[o:end]

    def section_of(self, vaddr):
        for name, s in self.sections.items():
            if s["addr"] and s["addr"] <= vaddr < s["addr"] + s["size"]:
                return name
        return None

    def dynsyms(self):
        """{name: (value, size)} of the defined dynamic symbols."""
        d, st = self.sections[".dynsym"], self.sections[".dynstr"]
        out = {}
        for i in range(d["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.data, d["off"] + i * 16)
            if shndx:
                out[self.cstr_at(st["off"] + name).decode("latin1")] = (value, size)
        return out


def phrasebook(path):
    """[(id, zh, en)] of a libUIPublic.so, as bytes (UTF-8 in the stock files), in index order."""
    e = Elf(path)
    va, size = e.dynsyms()["s_Items"]
    rows = []
    for i in range(size // 8):
        ident, row = struct.unpack_from("<iI", e.data, e.off(va + i * 8))
        if ident != i:
            raise ValueError("%s: s_Items[%d] has id %d" % (path, i, ident))
        zh, en = e.word(row), e.word(row + 4)
        rows.append((ident, e.cstr(zh), e.cstr(en)))
    return rows


def main(argv):
    if len(argv) >= 1 and argv[0] == "unpack":
        want = [a for a in argv[1:] if a != "--base"] or list(BASE_NAMES)
        for base in want:
            print("%s -> %s" % (base, os.path.relpath(unpack(base), WORK)))
    elif len(argv) == 2 and argv[0] == "dump":
        for ident, zh, en in phrasebook(argv[1]):
            print("%d\t%s\t%s" % (ident, zh.decode("utf-8").replace("\n", "\\n"), en.decode("utf-8").replace("\n", "\\n")))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
