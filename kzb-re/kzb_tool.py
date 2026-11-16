#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
kzb_tool.py - Parser / repacker for the Kanzi 2.8.4 .KZB binary directory
container (Desay "wayland-desay" port, big-endian / ARM target).

Reverse-engineered from libUICore-kanzi.so:
  kzuBinaryDirectoryCreateFromSource_internal  (header + directory block)
  kzuBinaryDirectoryReadFolder_internal        (recursive folder/file tree)
  kzuBinaryReadString                          (string encoding)

The container layout (all multi-byte integers BIG-ENDIAN):

  HEADER (offset 0 .. header_end):
    0x00  char[4]  magic ".KZB"  (2e 4b 5a 42)
    0x04  u32      total_file_size   (self describing)
    0x08  u32      header_end        (offset where the directory block starts;
                                       loader skips here after reading the name)
    0x0c  u32      version           (107 in these files; loader supports <=107)
    0x10  u16      schema0  (0x0002)
    0x12  u16      schema1  (0x0008)
    0x14  u16      schema2  (0x0004)
    0x16  u16      schema3  (0x0102)
    0x18  u32      flags_e  (1)
    0x1c  u32      field_f  (1)       (read, then ignored by loader)
    0x20  u32      uuid_len          (36)
    0x24  u8[uuid_len]  uuid_bytes   (project UUID, stored XOR-0x80; loader
                                       only skips it - kept verbatim here)
    ....  s32      type_code         (in {10,20,30,40,45,255})
    ....  STRING   name              (project name, plain ASCII)
    ....  pad       (skip to header_end)

  DIRECTORY BLOCK (header_end .. header_end + dir_size):
    u32     dir_size                 (size of this whole block)
    u32     num_files                (size of the file-pointer array - 1)
    FOLDER  root                     (recursive, see read_folder)
    u32     num_extra                ; num_extra * STRING   (appended refs)
    u32     num_deleted              ; num_deleted * STRING (deleted-file set)
    pad     (skip to header_end + dir_size)

  FOLDER:
    u32     folder_size              (byte size of this folder record)
    STRING  name
    u32     num_subfolders ; num_subfolders * FOLDER
    u32     num_files      ; num_files * FILE
    pad     (skip to folder start + folder_size)

  FILE:
    u32     offset   (absolute byte offset of the resource blob in the file)
    u32     size     (byte size of the resource blob)
    s32     type     (resource type enum, e.g. 1000)
    u32     flags    (bit2=cached, bit4=cubemap, bit5=premultiplied alpha)
    STRING  name

  DATA REGION (header_end + dir_size .. EOF):
    raw resource blobs, referenced by FILE.offset / FILE.size.

  STRING encoding (kzuBinaryReadString):
    u16  length
      if length == 0xFFFF -> NULL string (2 bytes total, no data, no padding)
      else length bytes, then padding so that (2 + length) is 4-byte aligned.

This tool round-trips a .KZB byte-for-byte: parse(data) then build(model)
reproduces the exact input (verified via md5). Resource blob contents are kept
as an opaque span of raw bytes; everything in the header and the directory
block is decoded into fields and rebuilt from them.
"""

import struct
import sys
import hashlib
import json

MAGIC = b".KZB"


class Reader:
    __slots__ = ("d", "p")

    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from(">H", self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def s32(self):
        v = struct.unpack_from(">i", self.d, self.p)[0]
        self.p += 4
        return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v


def _str_pad_len(length):
    # kzuBinaryReadString: skip so that (2 + length) is aligned to 4.
    total = 2 + length
    return (-total) & 3


def read_string(r):
    """Returns a dict describing the string record (for lossless rebuild)."""
    length = r.u16()
    if length == 0xFFFF:
        return {"null": True}
    data = r.raw(length)
    pad = r.raw(_str_pad_len(length))
    return {"len": length, "data": data, "pad": pad}


def write_string(out, s):
    if s.get("null"):
        out += struct.pack(">H", 0xFFFF)
        return
    out += struct.pack(">H", s["len"])
    out += s["data"]
    out += s["pad"]


def read_folder(r):
    start = r.p
    folder_size = r.u32()
    name = read_string(r)
    num_subfolders = r.u32()
    subfolders = [read_folder(r) for _ in range(num_subfolders)]
    num_files = r.u32()
    files = []
    for _ in range(num_files):
        offset = r.u32()
        size = r.u32()
        ftype = r.s32()
        flags = r.u32()
        fname = read_string(r)
        files.append({
            "offset": offset, "size": size, "type": ftype,
            "flags": flags, "name": fname,
        })
    consumed = r.p - start
    pad = r.raw(folder_size - consumed)
    return {
        "folder_size": folder_size,
        "name": name,
        "subfolders": subfolders,
        "files": files,
        "pad": pad,
    }


def write_folder(out, f):
    start = len(out)
    out += struct.pack(">I", f["folder_size"])
    write_string(out, f["name"])
    out += struct.pack(">I", len(f["subfolders"]))
    for sf in f["subfolders"]:
        write_folder(out, sf)
    out += struct.pack(">I", len(f["files"]))
    for fi in f["files"]:
        out += struct.pack(">IIiI", fi["offset"], fi["size"], fi["type"], fi["flags"])
        write_string(out, fi["name"])
    consumed = len(out) - start
    assert consumed <= f["folder_size"], (consumed, f["folder_size"])
    out += f["pad"]


def parse(data):
    r = Reader(data)
    magic = r.raw(4)
    if magic != MAGIC:
        raise ValueError("bad magic: %r" % magic)
    total_size = r.u32()
    header_end = r.u32()
    version = r.u32()
    schema = [r.u16(), r.u16(), r.u16(), r.u16()]
    flags_e = r.u32()
    field_f = r.u32()
    uuid_len = r.u32()
    uuid_bytes = r.raw(uuid_len)
    type_code = r.s32()
    name = read_string(r)
    header_pad = r.raw(header_end - r.p)

    # ---- directory block ----
    dir_start = r.p                      # == header_end
    dir_size = r.u32()
    num_files = r.u32()
    root = read_folder(r)

    num_extra = r.u32()
    extra = [read_string(r) for _ in range(num_extra)]

    num_deleted = r.u32()
    deleted = [read_string(r) for _ in range(num_deleted)]

    consumed = r.p - dir_start
    dir_pad = r.raw(dir_size - consumed)

    data_region = r.d[r.p:]

    model = {
        "total_size": total_size,
        "header_end": header_end,
        "version": version,
        "schema": schema,
        "flags_e": flags_e,
        "field_f": field_f,
        "uuid_len": uuid_len,
        "uuid_bytes": uuid_bytes,
        "type_code": type_code,
        "name": name,
        "header_pad": header_pad,
        "dir_size": dir_size,
        "num_files": num_files,
        "root": root,
        "extra": extra,
        "deleted": deleted,
        "dir_pad": dir_pad,
        "data_region": data_region,
        "data_region_offset": r.p,
    }
    return model


def build(model):
    out = bytearray()
    out += MAGIC
    out += struct.pack(">I", model["total_size"])
    out += struct.pack(">I", model["header_end"])
    out += struct.pack(">I", model["version"])
    for h in model["schema"]:
        out += struct.pack(">H", h)
    out += struct.pack(">I", model["flags_e"])
    out += struct.pack(">I", model["field_f"])
    out += struct.pack(">I", model["uuid_len"])
    out += model["uuid_bytes"]
    out += struct.pack(">i", model["type_code"])
    write_string(out, model["name"])
    out += model["header_pad"]

    dir_start = len(out)
    assert dir_start == model["header_end"], (dir_start, model["header_end"])
    out += struct.pack(">I", model["dir_size"])
    out += struct.pack(">I", model["num_files"])
    write_folder(out, model["root"])
    out += struct.pack(">I", len(model["extra"]))
    for s in model["extra"]:
        write_string(out, s)
    out += struct.pack(">I", len(model["deleted"]))
    for s in model["deleted"]:
        write_string(out, s)
    consumed = len(out) - dir_start
    assert consumed <= model["dir_size"], (consumed, model["dir_size"])
    out += model["dir_pad"]

    assert len(out) == model["data_region_offset"], (len(out), model["data_region_offset"])
    out += model["data_region"]
    return bytes(out)


# ----------------------------------------------------------------------------
# authoring: enumeration, size recompute, prefab clone
# ----------------------------------------------------------------------------
# Reference model (reverse-engineered from the loader):
#   kzuBinaryDirectoryReadReference reads a u32 index; kzuBinarySourceGetReferenced-
#   FilePath returns  directory->fileArray[index].  The file-pointer array is:
#       [0]      = NULL
#       [1 .. F] = embedded files, in loader DFS order (SUBFOLDERS FIRST, then
#                  own files; recursive) -- see kzuBinaryDirectoryReadFolder_internal
#       [F+1 ..] = the "extra" reference strings (num_extra)
#   Resource->resource references (material->texture, node->mesh, composer, ...)
#   are stored inside blobs as inline kzb:// / kzResID URL STRINGS, not indices.
#   Index references observed inside blobs: prefab-template -> instance-root, and
#   mesh -> material. Object-node parent/child structure comes from the FOLDER
#   hierarchy, not from in-blob references.

def _strlen(s):
    return 2 if s.get("null") else 2 + s["len"] + len(s["pad"])


def _pad_for(length):
    return b"\x00" * ((-(2 + length)) & 3)


def make_string(text):
    """Build a string record dict from a python str/bytes (plain, 4-aligned)."""
    if text is None:
        return {"null": True}
    data = text.encode("latin-1") if isinstance(text, str) else bytes(text)
    return {"len": len(data), "data": data, "pad": _pad_for(len(data))}


def count_files(folder):
    n = len(folder["files"])
    for sf in folder["subfolders"]:
        n += count_files(sf)
    return n


def enumerate_loader_order(model):
    """Return [(path, fileinfo)] in the loader's file-array order (index = pos+1)."""
    out = []

    def walk(folder, prefix):
        path = prefix + (_decode_name(folder["name"]) or "")
        for sf in folder["subfolders"]:
            walk(sf, path + "/")
        for fi in folder["files"]:
            out.append((path + "/" + (_decode_name(fi["name"]) or ""), fi))

    walk(model["root"], "")
    return out


def _recompute_folder(f):
    n = 4 + _strlen(f["name"]) + 4
    for sf in f["subfolders"]:
        n += _recompute_folder(sf)
    n += 4
    for fi in f["files"]:
        n += 16 + _strlen(fi["name"])
    n += len(f["pad"])
    f["folder_size"] = n
    return n


def recompute_sizes(model):
    """Recompute folder_size, dir_size, num_files, data_region_offset, total_size
    from the current tree (pads preserved). Idempotent on an unmodified model."""
    _recompute_folder(model["root"])
    dc = 4 + 4 + model["root"]["folder_size"]
    dc += 4 + sum(_strlen(s) for s in model["extra"])
    dc += 4 + sum(_strlen(s) for s in model["deleted"])
    dc += len(model["dir_pad"])
    model["dir_size"] = dc
    model["data_region_offset"] = model["header_end"] + dc
    model["num_files"] = count_files(model["root"]) + len(model["extra"])
    model["total_size"] = model["data_region_offset"] + len(model["data_region"])
    return model


def rewrite_blob_strings(blob, old, new):
    """Rewrite inline resource-reference URL strings inside a blob that contain
    `old`, replacing with `new` and fixing their stored length.

    Resource-URL value encoding (reverse-engineered):
        [u16 BE length][0x00][`length` ASCII bytes][0x00 terminator]
    The u16 length sits 3 bytes before the ASCII data (a 0x00 separator follows
    it). Returns (new_blob, count)."""
    import re
    old = old.encode() if isinstance(old, str) else old
    new = new.encode() if isinstance(new, str) else new
    runs = list(re.finditer(rb'(?:kzb://|kzResID)[ -~]+', blob))
    out = bytearray()
    pos = 0
    count = 0
    for mm in runs:
        run = mm.group()
        if old not in run:
            continue
        k, L = mm.start(), len(mm.group())
        if k < 3:
            continue
        stored = struct.unpack_from(">H", blob, k - 3)[0]
        if stored != L:
            # length field not where expected; skip (reported by caller/check)
            continue
        nrun = run.replace(old, new)
        out += blob[pos:k - 3]
        out += struct.pack(">H", len(nrun))
        out += blob[k - 1:k]          # 0x00 separator between length and data
        out += nrun
        pos = k + L
        count += 1
    out += blob[pos:]
    return bytes(out), count


def _be(v):
    return struct.pack(">I", v)


def _blob_of(model, fi):
    base = model["data_region_offset"]
    o = fi["offset"] - base
    return model["data_region"][o:o + fi["size"]]


def clone_prefab(model, src="CarPlayLayer_Prefab", dst="AndroidAutoLayer_Prefab",
                 new_material="TexturedMaterial_Androidauto", verbose=True):
    """Clone the /Prefabs/<src> prefab subtree (folder + its type-2080 template
    file) into /Prefabs/<dst>, repointing resource URLs to the clone, repointing
    the cloned primitive-mesh material to <new_material>, and fixing up intra-clone
    index references (template -> instance-root, etc.). Appended at the end of
    /Prefabs; all existing blob offsets are shifted by the directory growth."""
    import copy

    old_base = model["data_region_offset"]

    # -- locate /Prefabs ----------------------------------------------------
    prefabs = None
    for sf in model["root"]["subfolders"]:
        if _decode_name(sf["name"]) == "Prefabs":
            prefabs = sf
            break
    if prefabs is None:
        raise ValueError("/Prefabs folder not found")

    src_folder = None
    for sf in prefabs["subfolders"]:
        if _decode_name(sf["name"]) == src:
            src_folder = sf
            break
    src_tpl = None
    for fi in prefabs["files"]:
        if _decode_name(fi["name"]) == src:
            src_tpl = fi
            break
    if src_folder is None or src_tpl is None:
        raise ValueError("source prefab %r not found under /Prefabs" % src)

    # -- old subtree indices (for intra-clone ref remap) --------------------
    old_order = enumerate_loader_order(model)
    old_idx = {p: i + 1 for i, (p, _) in enumerate(old_order)}
    src_prefix = "/Prefabs/" + src
    tpl_path = "/Prefabs/" + src  # template file shares the path string

    # material indices (in /Materials, before /Prefabs -> unaffected by shift)
    mat_idx = {}
    for p, fi in old_order:
        if p.startswith("/Materials/"):
            mat_idx[p.rsplit("/", 1)[-1]] = old_idx[p]
    new_mat_index = mat_idx.get(new_material)
    if new_mat_index is None:
        raise ValueError("material %r not found under /Materials" % new_material)

    # -- blob layout note ---------------------------------------------------
    # Blobs are laid out contiguously as [content][4-byte gap]; a FILE record's
    # real content runs from its offset up to the NEXT blob's offset, i.e.
    # next_offset == offset + size + 4. (FILE.size under-reports the content by
    # the 4-byte inter-blob gap.) So we clone gap-bounded content and keep
    # size = len(content) - 4 to preserve the layout rule exactly.
    by_off = sorted(((fi["offset"], fi) for _, fi in old_order))
    next_off = {}
    for i, (o, fi) in enumerate(by_off):
        next_off[id(fi)] = by_off[i + 1][0] if i + 1 < len(by_off) else model["total_size"]

    def gap_blob(fi):
        b = model["data_region_offset"]
        return model["data_region"][fi["offset"] - b: next_off[id(fi)] - b]

    # -- capture original blobs for every source file (keyed by path) -------
    src_blobs = {}
    for p, fi in old_order:
        if p == tpl_path or p.startswith(src_prefix + "/") or p == src_prefix:
            src_blobs[p] = gap_blob(fi)

    # -- deep-copy folder subtree + template, rename names ------------------
    def rename_names(folder):
        nm = _decode_name(folder["name"])
        if nm and src in nm:
            folder["name"] = make_string(nm.replace(src, dst))
        for fi in folder["files"]:
            fn = _decode_name(fi["name"])
            if fn and src in fn:
                fi["name"] = make_string(fn.replace(src, dst))
        for sf in folder["subfolders"]:
            rename_names(sf)

    new_folder = copy.deepcopy(src_folder)
    rename_names(new_folder)
    new_tpl = copy.deepcopy(src_tpl)
    new_tpl["name"] = make_string(dst)

    # -- insert into /Prefabs (append -> keeps all earlier indices stable) --
    prefabs["subfolders"].append(new_folder)
    prefabs["files"].append(new_tpl)

    # -- new subtree indices (after insertion) ------------------------------
    new_order = enumerate_loader_order(model)
    new_idx = {}
    for i, (p, fi) in enumerate(new_order):
        new_idx[p] = i + 1
    dst_prefix = "/Prefabs/" + dst

    # map old-subtree path -> (old_index, new_index) for intra-clone refs
    idx_map = {}
    for p in src_blobs:
        np = p.replace(src, dst)
        if p in old_idx and np in new_idx:
            idx_map[old_idx[p]] = new_idx[np]

    # -- build new blob bytes for each cloned file --------------------------
    # recompute offsets later; for now collect (fileinfo, newbytes)
    clone_files = []  # (path_new, fileinfo, newblob)

    def collect(folder, prefix):
        path = prefix + (_decode_name(folder["name"]) or "")
        for sf in folder["subfolders"]:
            collect(sf, path + "/")
        for fi in folder["files"]:
            clone_files.append((path + "/" + (_decode_name(fi["name"]) or ""), fi))

    collect(new_folder, "/Prefabs/")
    clone_files.append((dst_prefix, new_tpl))

    url_rewrites = 0
    mat_rewrites = 0
    ref_rewrites = 0
    for path_new, fi in clone_files:
        path_old = path_new.replace(dst, src)
        orig_size = fi["size"]          # FILE.size copied from source
        blob = bytearray(src_blobs[path_old])   # gap-bounded source content
        orig_len = len(blob)
        # 1) rewrite inline path/URL strings  src -> dst
        nb, c = rewrite_blob_strings(bytes(blob), src, dst)
        url_rewrites += c
        blob = bytearray(nb)
        # 2) material repoint: type-510 primitive mesh -> material index @ +6
        if fi["type"] == 510 and len(blob) >= 10:
            cur = struct.unpack_from(">I", blob, 6)[0]
            # only repoint if it currently points at a material resource
            if cur in [v for k, v in mat_idx.items()]:
                struct.pack_into(">I", blob, 6, new_mat_index)
                mat_rewrites += 1
        # 3) intra-clone index references (template->instance-root, etc.)
        for oi, ni in sorted(idx_map.items(), reverse=True):
            needle = _be(oi)
            if needle in blob and oi != ni:
                blob2, nrep = _replace_all(blob, needle, _be(ni))
                blob = bytearray(blob2)
                ref_rewrites += nrep
        # blob is gap-bounded content (incl. trailing inter-blob padding, kept
        # verbatim); FILE.size grows only by the bytes the rewrites added.
        fi["size"] = orig_size + (len(blob) - orig_len)
        fi["_newblob"] = bytes(blob)

    # -- lay out new blobs after the existing data region -------------------
    new_region = bytearray(model["data_region"])
    for path_new, fi in clone_files:
        fi["_rel"] = len(new_region)
        new_region += fi["_newblob"]
    model["data_region"] = bytes(new_region)

    # -- recompute directory sizes, then fix every offset -------------------
    recompute_sizes(model)
    new_base = model["data_region_offset"]
    delta = new_base - old_base
    clone_set = set(id(fi) for _, fi in clone_files)
    for _, fi in enumerate_loader_order(model):
        if id(fi) in clone_set:
            fi["offset"] = new_base + fi.pop("_rel")
            fi.pop("_newblob", None)
        else:
            fi["offset"] = fi["offset"] + delta
    recompute_sizes(model)  # total_size (offsets don't affect sizes)

    info = {
        "src": src, "dst": dst, "clone_file_count": len(clone_files),
        "url_rewrites": url_rewrites, "material_rewrites": mat_rewrites,
        "index_ref_rewrites": ref_rewrites, "new_material": new_material,
        "new_material_index": new_mat_index, "dir_growth": delta,
        "idx_map": idx_map,
    }
    if verbose:
        print("clone %s -> %s : %d files, %d url-rewrites, %d material-repoints, "
              "%d index-ref-rewrites, dir grew %d bytes" %
              (src, dst, len(clone_files), url_rewrites, mat_rewrites,
               ref_rewrites, delta))
    return info


def _replace_all(buf, old, new):
    assert len(old) == len(new)
    out = bytearray(buf)
    n = 0
    start = 0
    while True:
        k = out.find(old, start)
        if k < 0:
            break
        out[k:k + len(new)] = new
        start = k + len(new)
        n += 1
    return bytes(out), n


# ----------------------------------------------------------------------------
# dump helpers
# ----------------------------------------------------------------------------
def _decode_name(s):
    if s.get("null"):
        return None
    return s["data"].decode("latin-1")


def _iter_files(folder, prefix=""):
    path = prefix + (_decode_name(folder["name"]) or "")
    for fi in folder["files"]:
        yield path, fi
    for sf in folder["subfolders"]:
        yield from _iter_files(sf, path + "/")


def dump(model, show_files=True, limit=None):
    print("magic            : .KZB")
    print("total_size       : %d (0x%x)" % (model["total_size"], model["total_size"]))
    print("header_end       : 0x%x" % model["header_end"])
    print("version          : %d" % model["version"])
    print("schema           : %s" % " ".join("0x%04x" % x for x in model["schema"]))
    print("flags_e, field_f : %d, %d" % (model["flags_e"], model["field_f"]))
    print("uuid (xor^0x80)  : %s" % bytes(b ^ 0x80 for b in model["uuid_bytes"]).decode("latin-1"))
    print("type_code        : %d" % model["type_code"])
    print("project name     : %s" % _decode_name(model["name"]))
    print("dir_size         : %d (0x%x)" % (model["dir_size"], model["dir_size"]))
    print("num_files field  : %d" % model["num_files"])
    print("data region      : offset 0x%x, %d bytes" %
          (model["data_region_offset"], len(model["data_region"])))
    all_files = list(_iter_files(model["root"]))
    print("file records     : %d" % len(all_files))
    print("extra strings    : %d" % len(model["extra"]))
    print("deleted strings  : %d" % len(model["deleted"]))
    if show_files:
        print("--- files (path | offset | size | type | flags) ---")
        n = 0
        for path, fi in all_files:
            if limit is not None and n >= limit:
                print("  ... (%d more)" % (len(all_files) - n))
                break
            print("  %s/%s | 0x%x | %d | %d | 0x%x" %
                  (path, _decode_name(fi["name"]), fi["offset"], fi["size"],
                   fi["type"], fi["flags"]))
            n += 1
        if model["extra"]:
            print("--- extra strings (first 20) ---")
            for s in model["extra"][:20]:
                print("  %s" % _decode_name(s))


def md5(b):
    return hashlib.md5(b).hexdigest()


import re as _re
_URL_RE = _re.compile(rb'(?:kzb://[A-Za-z0-9_]+/|kzResID)([ -~]+)')


def check_structure(model, verbose=True):
    """Structural self-check of a (possibly modified) model:
      - every file blob lies inside the data region and does not overrun EOF
      - num_files field is consistent
      - build->parse->build is idempotent
      - every inline kzb:// URL inside every blob resolves to an existing file path
    Returns (ok, problems)."""
    problems = []
    order = enumerate_loader_order(model)
    base = model["data_region_offset"]
    total = model["total_size"]
    region = model["data_region"]

    # build the set of valid resource paths (loader form: no leading slash)
    valid_paths = set()
    for p, fi in order:
        valid_paths.add(p.lstrip("/"))

    # bounds + collect blobs
    for p, fi in order:
        o, s = fi["offset"], fi["size"]
        if o < base or o + s > total:
            problems.append("out-of-bounds blob %s off=0x%x size=%d (base=0x%x total=0x%x)"
                            % (p, o, s, base, total))

    # num_files consistency
    nf = count_files(model["root"]) + len(model["extra"])
    if nf != model["num_files"]:
        problems.append("num_files field %d != computed %d" % (model["num_files"], nf))

    # idempotent rebuild
    try:
        rebuilt = build(model)
        m2 = parse(rebuilt)
        rebuilt2 = build(m2)
        if rebuilt != rebuilt2:
            problems.append("NON-idempotent: build!=build(parse(build))")
    except Exception as e:  # noqa
        problems.append("rebuild/reparse failed: %r" % e)

    # inline kzb:// / kzResID URL resolution. Blobs are read gap-bounded (the
    # real content runs to the next blob; FILE.size under-reports by the 4-byte
    # inter-blob gap), else trailing URLs get truncated. Each URL value string is
    # [u16 BE len][0x00][len ASCII bytes], so we extract exactly `len` bytes
    # (a greedy scan would grab a trailing non-URL byte).
    by_off = sorted(((fi["offset"], fi) for _, fi in order))
    nxt = {}
    for i, (o, fi) in enumerate(by_off):
        nxt[id(fi)] = by_off[i + 1][0] if i + 1 < len(by_off) else total
    dangling = []
    checked = 0
    for p, fi in order:
        o = fi["offset"] - base
        blob = region[o:nxt[id(fi)] - base]
        for mk in _re.finditer(rb'kzb://|kzResID', blob):
            k = mk.start()
            if k < 3:
                continue
            L = struct.unpack_from(">H", blob, k - 3)[0]
            if L < 7 or k + L > len(blob):
                continue
            url = blob[k:k + L].decode("latin-1")
            checked += 1
            if url.startswith("kzb://"):
                path = url.split("/", 3)[-1] if url.count("/") >= 3 else ""
            else:  # kzResID<path>
                path = url[len("kzResID"):]
            if path.startswith("<") or path in valid_paths:
                continue
            dangling.append((p, url[:90]))
    if dangling:
        problems.append("%d inline URL(s) did not resolve to a file path" % len(dangling))

    ok = not problems
    if verbose:
        print("structural check: %s" % ("OK" if ok else "PROBLEMS"))
        print("  files=%d  extra=%d  kzb://URLs checked=%d  dangling=%d"
              % (len(order), len(model["extra"]), checked, len(dangling)))
        for pr in problems:
            print("  ! " + pr)
        for dp, du in dangling[:10]:
            print("  dangling URL in %s -> kzb://.../%s" % (dp, du))
    return ok, problems


def _find(model, path):
    for p, fi in enumerate_loader_order(model):
        if p == path:
            return fi
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        print("usage:")
        print("  kzb_tool.py dump    <file.kzb> [--files]")
        print("  kzb_tool.py verify  <file.kzb>          (round-trip md5 check)")
        print("  kzb_tool.py selftest <file.kzb> ...")
        print("  kzb_tool.py check   <file.kzb>          (structural + URL resolve)")
        print("  kzb_tool.py clone-prefab <in.kzb> <out.kzb> [src] [dst] [material]")
        return 2
    cmd = argv[1]
    if cmd == "check":
        with open(argv[2], "rb") as f:
            data = f.read()
        m = parse(data)
        ok, _ = check_structure(m)
        return 0 if ok else 1
    if cmd == "clone-prefab":
        inp, outp = argv[2], argv[3]
        src = argv[4] if len(argv) > 4 else "CarPlayLayer_Prefab"
        dst = argv[5] if len(argv) > 5 else "AndroidAutoLayer_Prefab"
        mat = argv[6] if len(argv) > 6 else "TexturedMaterial_Androidauto"
        with open(inp, "rb") as f:
            data = f.read()
        m = parse(data)
        clone_prefab(m, src=src, dst=dst, new_material=mat)
        out = build(m)
        with open(outp, "wb") as f:
            f.write(out)
        print("wrote %s (%d bytes, md5=%s)" % (outp, len(out), md5(out)))
        # self re-parse + idempotency
        m2 = parse(out)
        out2 = build(m2)
        print("re-parse idempotent: %s" % ("YES" if out2 == out else "NO"))
        check_structure(m2)
        return 0
    if cmd == "dump":
        with open(argv[2], "rb") as f:
            data = f.read()
        m = parse(data)
        dump(m, show_files=("--files" in argv), limit=None if "--files" in argv else 0)
        return 0
    if cmd == "verify":
        with open(argv[2], "rb") as f:
            data = f.read()
        m = parse(data)
        rebuilt = build(m)
        ok = rebuilt == data
        print("%s  parse->build  input md5=%s  rebuilt md5=%s  %s" %
              (argv[2], md5(data), md5(rebuilt), "IDENTICAL" if ok else "MISMATCH"))
        if not ok:
            # locate first diff
            n = min(len(data), len(rebuilt))
            for i in range(n):
                if data[i] != rebuilt[i]:
                    print("  first diff at 0x%x" % i)
                    break
            print("  len in=%d out=%d" % (len(data), len(rebuilt)))
        return 0 if ok else 1
    if cmd == "selftest":
        rc = 0
        for path in argv[2:]:
            with open(path, "rb") as f:
                data = f.read()
            m = parse(data)
            rebuilt = build(m)
            ok = rebuilt == data
            rc = rc or (0 if ok else 1)
            print("%-28s %s  md5=%s" %
                  (path.split("/")[-1], "OK " if ok else "FAIL", md5(rebuilt)))
        return rc
    print("unknown command: %s" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
