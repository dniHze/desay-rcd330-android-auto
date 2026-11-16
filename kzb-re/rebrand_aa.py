#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rebrand_aa.py - "Strategy A" rebrand of the AppConnect CarLife menu tile to the
stock Android Auto tile art that already ships in the same container.

    python3 -I kzb-re/rebrand_aa.py <stock res/VW_MP2017.kzb> <out.kzb> [--report out.json]

What it does (and only this):
  * In each target blob, parses the trailing Kanzi resource dictionary
    ([u32 LE count] then count x ([STRING key][STRING value]); STRING =
    [u16 LE len][bytes][zero pad so (2+len) % 4 == 0]) strictly, requiring it to
    consume the blob to its end.
  * Rewrites the image URLs listed in URL_MAP (CarLife -N/-P/-P_Glow and the
    -5_* variants) to the stock Android Auto textures (-N/-P/-P_Glow). No pixel
    data, texture, material, node, name or directory string is touched.
  * Appends each rewritten blob at the end of the data region and repoints only
    that FILE record (offset = new position, size += content delta). Every other
    byte of the stock file stays at its original offset; the superseded stock
    bytes stay in place, unreferenced.
  * recompute_sizes() (directory sizes must come out unchanged; total_size grows).

Why not kzb_tool.rewrite_blob_strings(): it models an in-blob string as a
big-endian u16 three bytes before the text. The actual encoding (above) is a
little-endian u16 directly before the text plus 4-byte padding, so that helper
silently skips any string whose preceding byte is non-zero (e.g. the URL after
"PressImage") and never fixes the padding.

Run with python3 -I on firmware files. Never modifies its input.
"""
import hashlib
import importlib.util
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("kzb_tool", os.path.join(HERE, "kzb_tool.py"))
kt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kt)

# Known stock res/VW_MP2017.kzb builds: the firmware dump's, the one the unit actually runs
# (saved by step 3 to aa-dev/unit-files/; it differs from the dump), and the MQB 0394 image's
# (its CarLife, Android Auto and logo image entries are byte-identical to the 0358 one's).
STOCK_MD5 = {"4afb7eb8c69c058335eb4058a4d6e546", "f394d0bc6fc903f27b57d68226030c34",
             "daa2ff81e531f4fa0d08211edb399287"}

T = "kzb://vw_mp2017/Textures/Mat_AppConnect_"
URL_MAP = {
    # menu-tile set (196x122)            -> stock AA tile art (196x122)
    T + "AnBaiduCarLife-N":        T + "Android_Auto-N",
    T + "BaiduCarLife-P":          T + "Android_Auto-P",
    T + "AnBaiduCarLife-P_Glow":   T + "Android_Auto-P_Glow",
    # "-5" layout variant (also 196x122) -> same AA art (no AA -5 variant ships)
    T + "AnBaiduCarLife-5_N":      T + "Android_Auto-N",
    T + "BaiduCarLife-5_P":        T + "Android_Auto-P",
    T + "AnBaiduCarLife-5_P_Glow": T + "Android_Auto-P_Glow",
}

TARGETS = [
    # live: SkMultiStateButtonCtrl tiles built by CAppconnectMainDlg::LoadButton()
    "/Prefabs/Node_Connect/Node_Connect/Btn_Carlife_Change/0",
    "/Prefabs/Node_Connect/Node_Connect/Btn_Carlife_Change/3",
    "/Prefabs/Node_Connect/Node_Connect/Btn_Carlife_2_Change/0",
    "/Prefabs/Node_Connect/Node_Connect/Btn_Carlife_2_Change/3",
    # copies of the same tile art in button prefabs that nothing references
    # (no node blob and no appconnect code names them); changed for consistency
    "/Prefabs/Btn_AppConnect_Baidu1_4_Prefab/Btn_AppConnect_Baidu1_4_Prefab",
    "/Prefabs/Btn_AppConnect_Baidu1_5_Prefab/Btn_AppConnect_Baidu1_5_Prefab",
    "/Prefabs/Btn_AppConnect_Baidu2_4_Prefab/Btn_AppConnect_Baidu2_4_Prefab",
    "/Prefabs/Btn_AppConnect_Baidu2_5_Prefab/Btn_AppConnect_Baidu2_5_Prefab",
]
IMAGE_KEYS = {b"DisableImage", b"PressImage", b"NormalImage", b"FocusImage", b"SelectImage"}


# ---------------------------------------------------------------- blob strings
def _pad(n):
    return (-(2 + n)) & 3


def ser_str(s):
    return struct.pack("<H", len(s)) + s + b"\0" * _pad(len(s))


def parse_trailing_dict(b, p):
    """Strictly parse a resource dictionary starting at p that must run to the
    end of blob b. Returns (pairs, last_data_end, tail) or None."""
    if p + 4 > len(b):
        return None
    n = struct.unpack_from("<I", b, p)[0]
    if not 1 <= n <= 32:
        return None
    q = p + 4
    items = []
    for i in range(2 * n):
        if q + 2 > len(b):
            return None
        L = struct.unpack_from("<H", b, q)[0]
        if L == 0xFFFF or q + 2 + L > len(b):
            return None
        s = b[q + 2:q + 2 + L]
        if not s or not all(0x20 <= c < 0x7f for c in s):
            return None
        items.append(s)
        q += 2 + L
        if i < 2 * n - 1:
            pd = _pad(L)
            if b[q:q + pd] != b"\0" * pd:
                return None
            q += pd
    tail = b[q:]
    if tail.strip(b"\0") or len(tail) > 8:
        return None
    pairs = list(zip(items[0::2], items[1::2]))
    return pairs, q, tail


def find_image_dict(b):
    hits = []
    for p in range(0, len(b) - 4):
        r = parse_trailing_dict(b, p)
        if r and all(k in IMAGE_KEYS for k, _ in r[0]):
            hits.append((p, r))
    if len(hits) != 1:
        raise ValueError("expected exactly one trailing image dictionary, found %d" % len(hits))
    return hits[0]


def rewrite_blob(b):
    """Returns (new_blob, [(key, old_url, new_url)]) ."""
    p, (pairs, last_end, tail) = find_image_dict(b)
    changes, new_pairs = [], []
    for k, v in pairs:
        vs = v.decode("ascii")
        if vs in URL_MAP:
            nv = URL_MAP[vs].encode("ascii")
            changes.append((k.decode(), vs, URL_MAP[vs]))
        else:
            if re.search(r"(?i)carlife|baidu", vs):
                raise ValueError("unmapped CarLife URL %s" % vs)
            nv = v
        new_pairs.append((k, nv))
    # last string: keep stock convention for its trailing pad (present or not)
    old_last = pairs[-1][1]
    pd_old = _pad(len(old_last))
    pad_present = len(tail) >= pd_old and tail[:pd_old] == b"\0" * pd_old
    extra = tail[pd_old:] if pad_present else tail
    out = bytearray(b[:p])
    out += struct.pack("<I", len(new_pairs))
    for i, (k, v) in enumerate(new_pairs):
        out += ser_str(k)
        if i < len(new_pairs) - 1:
            out += ser_str(v)
        else:
            out += struct.pack("<H", len(v)) + v
            if pad_present:
                out += b"\0" * _pad(len(v))
            out += extra
    return bytes(out), changes


# ---------------------------------------------------------------- model helpers
def gap_bounds(model):
    order = kt.enumerate_loader_order(model)
    by_off = sorted((fi["offset"], id(fi)) for _, fi in order)
    nxt = {}
    for i, (o, k) in enumerate(by_off):
        nxt[k] = by_off[i + 1][0] if i + 1 < len(by_off) else model["total_size"]
    return order, nxt


def blob_gap(data, fi, nxt):
    return data[fi["offset"]:nxt[id(fi)]]


def flatten(model):
    """Every directory-level record as {key: value} for diffing."""
    rec = {}
    for k in ("total_size", "header_end", "version", "flags_e", "field_f", "uuid_len",
              "type_code", "dir_size", "num_files", "data_region_offset"):
        rec["header." + k] = model[k]
    rec["header.schema"] = tuple(model["schema"])
    rec["header.uuid_bytes"] = bytes(model["uuid_bytes"])
    rec["header.name"] = kt._decode_name(model["name"])
    rec["header.pad"] = bytes(model["header_pad"])
    rec["dir.pad"] = bytes(model["dir_pad"])
    rec["extra"] = tuple(kt._decode_name(s) for s in model["extra"])
    rec["deleted"] = tuple(kt._decode_name(s) for s in model["deleted"])

    def walk(f, prefix):
        path = prefix + (kt._decode_name(f["name"]) or "")
        rec["folder:" + (path or "/")] = (f["folder_size"], len(f["subfolders"]), len(f["files"]), bytes(f["pad"]))
        for sf in f["subfolders"]:
            walk(sf, path + "/")
        for fi in f["files"]:
            rec["file:" + path + "/" + kt._decode_name(fi["name"])] = (
                fi["offset"], fi["size"], fi["type"], fi["flags"])
    walk(model["root"], "")
    return rec


def le_url_check(data, model):
    """Resolve every inline kzb:// URL using the real LE string encoding."""
    order, nxt = gap_bounds(model)
    valid = {p.lstrip("/") for p, _ in order}
    checked, dangling = 0, []
    for p, fi in order:
        b = blob_gap(data, fi, nxt)
        for mk in re.finditer(rb"kzb://", b):
            k = mk.start()
            if k < 2:
                continue
            L = struct.unpack_from("<H", b, k - 2)[0]
            if L < 7 or k + L > len(b):
                continue
            url = b[k:k + L]
            if not all(0x20 <= c < 0x7f for c in url):
                continue
            checked += 1
            path = url.decode().split("/", 3)[-1]
            if path not in valid:
                dangling.append((p, url.decode()))
    return checked, dangling


def md5(b):
    return hashlib.md5(b).hexdigest()


# ---------------------------------------------------------------- main
def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    report_path = argv[argv.index("--report") + 1] if "--report" in argv else None
    stock = open(src, "rb").read()
    rep = {"stock": src, "stock_md5": md5(stock), "stock_size": len(stock)}
    assert md5(stock) in STOCK_MD5, "unexpected input md5 %s" % md5(stock)

    # gate: parse -> build must reproduce the stock file byte-for-byte
    m0 = kt.parse(stock)
    assert kt.build(m0) == stock, "round-trip gate FAILED"
    rep["roundtrip_stock"] = "IDENTICAL"
    stock_flat = flatten(m0)
    _, stock_problems = kt.check_structure(m0, verbose=False)
    stock_le = le_url_check(stock, m0)

    m = kt.parse(stock)
    order, nxt = gap_bounds(m)
    fis = dict(order)
    assert len(fis) == len(order), "duplicate file paths"
    region = bytearray(m["data_region"])
    base = m["data_region_offset"]
    edits = []
    for path in TARGETS:
        fi = fis[path]
        old = blob_gap(stock, fi, nxt)
        new, changes = rewrite_blob(old)
        # validate the rewritten blob with the same strict parser
        _, (pairs, _, _) = find_image_dict(new)
        for k, v in pairs:
            assert v.decode() not in URL_MAP, (path, v)
        delta = len(new) - len(old)
        e = {"path": path, "old_offset": fi["offset"], "old_size": fi["size"],
             "old_content_len": len(old), "new_content_len": len(new), "delta": delta,
             "url_changes": changes}
        fi["offset"] = base + len(region)
        fi["size"] = fi["size"] + delta
        region += new
        e["new_offset"], e["new_size"] = fi["offset"], fi["size"]
        edits.append(e)
    m["data_region"] = bytes(region)

    before = (m["dir_size"], m["num_files"], m["root"]["folder_size"], m["data_region_offset"])
    kt.recompute_sizes(m)
    after = (m["dir_size"], m["num_files"], m["root"]["folder_size"], m["data_region_offset"])
    assert before == after, ("directory sizes changed", before, after)
    out = kt.build(m)
    assert len(out) == m["total_size"]

    # ---- validation on the written bytes
    m2 = kt.parse(out)
    assert kt.build(m2) == out, "re-parse/build not idempotent"
    ok2, problems2 = kt.check_structure(m2, verbose=False)
    new_le = le_url_check(out, m2)
    # every stock data byte unchanged and at its original offset
    assert out[base:len(stock)] == stock[base:], "stock data region bytes changed"
    # directory bytes differ only inside the edited FILE records + total_size
    new_flat = flatten(m2)
    diff = []
    for k in sorted(set(stock_flat) | set(new_flat)):
        if stock_flat.get(k) != new_flat.get(k):
            diff.append((k, stock_flat.get(k), new_flat.get(k)))
    allowed = {"header.total_size"} | {"file:" + p for p in TARGETS}
    unexpected = [d for d in diff if d[0] not in allowed]
    assert not unexpected, unexpected
    # rewritten blobs re-read from the output and re-parsed
    order2, nxt2 = gap_bounds(m2)
    fis2 = dict(order2)
    for path in TARGETS:
        b = blob_gap(out, fis2[path], nxt2)
        _, (pairs, _, _) = find_image_dict(b)
        for k, v in pairs:
            tgt = "/" + v.decode().split("/", 3)[-1]
            assert tgt in fis2 and fis2[tgt]["type"] == 2020, (path, v)
    # AA textures point at the AA images
    tex_img = {}
    for t in ("Android_Auto-N", "Android_Auto-P", "Android_Auto-P_Glow"):
        tb = blob_gap(out, fis2["/Textures/Mat_AppConnect_" + t], nxt2)
        idx = struct.unpack_from("<I", tb, 0x1d)[0]
        tex_img[t] = order2[idx - 1][0]
    open(dst, "wb").write(out)

    rep.update({
        "out": dst, "out_md5": md5(out), "out_size": len(out),
        "edits": edits,
        "model_diff": [{"record": k, "stock": repr(a), "new": repr(b)} for k, a, b in diff],
        "check_structure_stock": stock_problems, "check_structure_new": problems2,
        "check_structure_same_as_stock": problems2 == stock_problems,
        "le_url_check_stock": {"checked": stock_le[0], "dangling": stock_le[1]},
        "le_url_check_new": {"checked": new_le[0], "dangling": new_le[1]},
        "reparse_idempotent": True, "stock_data_bytes_unchanged": True,
        "aa_texture_images": tex_img,
    })
    print("stock  %s  md5=%s  %d bytes  round-trip IDENTICAL" % (src, rep["stock_md5"], len(stock)))
    for e in edits:
        print("edit   %s\n       off 0x%x->0x%x  size %d->%d  (content %+d)" %
              (e["path"], e["old_offset"], e["new_offset"], e["old_size"], e["new_size"], e["delta"]))
        for k, a, b in e["url_changes"]:
            print("         %-12s %s -> %s" % (k, a.rsplit("/", 1)[1], b.rsplit("/", 1)[1]))
    print("model diff: %d changed records (all expected)" % len(diff))
    for k, a, b in diff:
        print("   %s: %s -> %s" % (k, a, b))
    print("check_structure stock: %d problem(s); new: %d problem(s); identical: %s" %
          (len(stock_problems), len(problems2), problems2 == stock_problems))
    print("LE URL check  stock: %d checked, %d dangling; new: %d checked, %d dangling" %
          (stock_le[0], len(stock_le[1]), new_le[0], len(new_le[1])))
    print("AA textures -> images: %s" % tex_img)
    print("wrote  %s  md5=%s  %d bytes" % (dst, rep["out_md5"], len(out)))
    if report_path:
        with open(report_path, "w") as f:
            json.dump(rep, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
