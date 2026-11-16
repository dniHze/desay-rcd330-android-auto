#!/usr/bin/env python3
"""Read the RCF method-ID table out of compiled RcfClient stubs.

usage: rcf_ids.py <objdump -dr output of an object built at -O0 -fno-inline> ...

Each RcfClient<I>::Method(RCF::CallOptions const&, ...) stub constructs
RCF::FutureImpl<R>(..., interfaceName, fnId, ...); at -O0 the fnId is the literal stored to
[sp] just before that call, and the parameter types are in the AllocateClientParameters<>
call. Prints "<interface>\t<id>\t<method>\tsig<R, A1, ...>" sorted, one line per method,
in the same format as stock_ids.txt (which was read from the stock binaries).

Run with python3 -I. It only parses text produced by objdump on our own build.
Set CXXFILT to the demangler to use (default: c++filt).
"""
import os
import re
import subprocess
import sys


def demangle(names):
    cxxfilt = os.environ.get("CXXFILT", "c++filt")
    out = subprocess.run([cxxfilt], input="\n".join(names), capture_output=True, text=True)
    return out.stdout.splitlines()


def main(paths):
    found = {}
    for path in paths:
        cur = pending = fnid = alloc = None
        with open(path) as f:
            for line in f:
                m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
                if m:
                    cur, pending, fnid, alloc = m.group(1), None, None, None
                    continue
                if not cur or not cur.startswith("_ZN9RcfClientI") or "CallOptions" not in cur:
                    continue
                s = line.strip()
                # (objdump adds "; 0x27" after larger immediates)
                m = re.search(r"mov\s+r3, #(\d+)(\s*;.*)?$", s)
                if m:
                    pending = int(m.group(1))
                    continue
                if re.search(r"str\s+r3, \[sp\]$", s) and pending is not None:
                    fnid = pending
                    continue
                m = re.search(r"R_ARM_CALL\s+(\S+)", line)
                if m:
                    sym = m.group(1)
                    if "AllocateClientParameters" in sym:
                        alloc = sym
                    if sym.startswith("_ZN3RCF10FutureImpl") and ("C1E" in sym or "C2E" in sym):
                        found[cur] = (fnid, alloc)

    keys = list(found)
    names = demangle(keys)
    allocs = demangle([found[k][1] or "" for k in keys])
    rows = set()
    for key, name, alloc in zip(keys, names, allocs):
        m = re.search(r"RcfClient<(\w+)>::(\w+)\(", name)
        t = re.search(r"AllocateClientParameters<(.*?)>::", alloc)
        sig = re.sub(r"(, RCF::Void)+$", "", t.group(1)) if t else "?"
        sig = sig.replace("SVPCarLife::", "")
        # GNU c++filt spells out std::string; llvm's (used for stock_ids.txt) does not.
        sig = re.sub(r"std::basic_string<char, std::char_traits<char>, std::allocator<char> ?>",
                     "std::string", sig)
        rows.add((m.group(1), found[key][0], m.group(2), "sig<%s>" % sig))
    for iface, fnid, method, sig in sorted(rows, key=lambda r: (r[0], r[1] if r[1] is not None else -1)):
        print("%s\t%s\t%s\t%s" % (iface, fnid, method, sig))


if __name__ == "__main__":
    main(sys.argv[1:])
