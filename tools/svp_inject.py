#!/usr/bin/env python3
"""Rebuild svp.bin (gzip'd /svp rootfs tar) from the stock one, changing only named members.

Every stock member is copied with its original header (owner 1000/0, mode, mtime, order),
so the result differs from stock only by the files passed here. Unlike untar + `tar c`,
this adds no root "." entry and rewrites no owners or modes.

  svp_inject.py STOCK_SVP_BIN OUT_SVP_BIN \
      --replace ./etc/init.sh=my-init.sh \
      --add ./bin/androidautoservice=aa-headless.armhf [--like ./bin/connectionservice]

--replace keeps the stock header (only size/mtime change); the member must exist.
--add appends a new regular file whose owner/mode copy the --like member; it must not exist.

The output is reproducible: the gzip header carries no file name and the stock header's time,
and replaced or added members get --mtime (default: that same time), so the same inputs give
the same bytes. Headers are written in the stock tar's format (GNU).
"""
import argparse
import gzip
import os
import struct
import sys
import tarfile


def pair(s):
    name, _, path = s.partition("=")
    if not name or not path:
        raise argparse.ArgumentTypeError("want MEMBER=LOCAL_PATH, got %r" % s)
    # Members are named as in the stock tar: "./dir/file", nothing to normalise away.
    parts = name.split("/")
    if parts[0] != "." or len(parts) < 2 or any(p in ("", ".", "..") for p in parts[1:]):
        raise argparse.ArgumentTypeError("member %r is not of the form ./dir/file" % name)
    if not os.path.isfile(path):
        raise argparse.ArgumentTypeError("%s: not a regular file" % path)
    return name, path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stock")
    ap.add_argument("out")
    ap.add_argument("--replace", type=pair, action="append", default=[])
    ap.add_argument("--add", type=pair, action="append", default=[])
    ap.add_argument("--like", default="./bin/connectionservice",
                    help="member whose owner/mode new files copy")
    ap.add_argument("--mtime", type=int, default=None,
                    help="time stamp of replaced and added members (default: the stock gzip header's)")
    a = ap.parse_args()

    with open(a.stock, "rb") as f:
        head = f.read(10)
    if head[:3] != b"\x1f\x8b\x08":
        sys.exit("%s: not a gzip file" % a.stock)
    stock_time = struct.unpack("<I", head[4:8])[0]
    mtime = stock_time if a.mtime is None else a.mtime

    replace = dict(a.replace)
    add = dict(a.add)
    names = [n for n, _ in a.replace + a.add]
    if len(set(names)) != len(names):
        sys.exit("a member is named more than once: %s" % sorted(n for n in names if names.count(n) > 1))
    seen = set()
    like = None
    with tarfile.open(a.stock, "r:gz") as src, \
            open(a.out, "wb") as raw, \
            gzip.GzipFile(filename="", mode="wb", compresslevel=6, fileobj=raw, mtime=stock_time) as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as dst:
        for m in src:
            seen.add(m.name)
            if m.name in add:
                sys.exit("--add %s: already in the stock tar (use --replace)" % m.name)
            if m.name == a.like:
                like = m
            if m.name in replace:
                if not m.isreg():
                    sys.exit("--replace %s: not a regular file in the stock tar" % m.name)
                path = replace[m.name]
                ti = src.getmember(m.name)
                ti.size = os.path.getsize(path)
                ti.mtime = mtime
                with open(path, "rb") as f:
                    dst.addfile(ti, f)
                print("replaced %s <- %s (%d bytes, mode %o, owner %d/%d)"
                      % (m.name, path, ti.size, ti.mode, ti.uid, ti.gid))
            elif m.isreg():
                dst.addfile(m, src.extractfile(m))
            else:
                dst.addfile(m)
        missing = set(replace) - seen
        if missing:
            sys.exit("--replace: not in the stock tar: %s" % ", ".join(sorted(missing)))
        if add and like is None:
            sys.exit("--like %s: not in the stock tar" % a.like)
        for name, path in a.add:
            ti = tarfile.TarInfo(name)
            ti.type = tarfile.REGTYPE
            ti.size = os.path.getsize(path)
            ti.mtime = mtime
            ti.mode, ti.uid, ti.gid = like.mode, like.uid, like.gid
            ti.uname, ti.gname = like.uname, like.gname
            with open(path, "rb") as f:
                dst.addfile(ti, f)
            print("added    %s <- %s (%d bytes, mode %o, owner %d/%d)"
                  % (name, path, ti.size, ti.mode, ti.uid, ti.gid))


if __name__ == "__main__":
    main()
