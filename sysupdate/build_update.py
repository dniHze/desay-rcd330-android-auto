#!/usr/bin/env python3
"""Build the Android Auto system update from a stock Desay MIB2 image.

    python3 -I sysupdate/build_update.py [--base 0317|0394] [--build-id ID] [--accept-new]
                                                                          (from the work dir)

Bases (BASES below), from the firmware catalogue (firmware/): 0317, the 6RD 035 187B
(PQ) image of 2020 the car runs, the default; 0394, the 5GG 035 280D (MQB) image of a test unit.
They are the same SVP platform: the services the shim talks to keep their method IDs, and
libcarlife is identical. 0358 (the 2018 PQ image the car ran before, 2026-11-15) is no longer
built; its lock entries stay as history.

Inputs:
  * the base's stock update container and its .md5 sidecar (--stock-dir), checked against the
    md5s pinned below;
  * our three binaries (shim, receiver, bridge) at their build paths, checked against
    sysupdate/RELEASE.lock: only builds that ran in the car go into an image. --accept-new
    records the current ones in the lock instead (for a test build, before the car run), with
    aa_appconnect (it changes with appc_patch.hpp), and the rebrand outputs of a base the lock
    has none for yet;
  * the receiver's version stamp, aa-headless.armhf.version (r2_musl.sh): "LIVI-<build date>-<LIVI
    commit> <md5>", which must name the receiver's md5; shipped as etc/aa_android_auto_version for
    Settings > System information;
  * the launcher files in sysupdate/svp/;
  * the translation sheet, i18n/strings.csv, for the UI languages.

What it does, all of it checked:
  1. unpacks the stock container and checks every component's md5;
  2. takes the stock files the rebrand starts from out of the image itself (AppConnect and its
     resource container from app-pkg.bin, libUIPublic.so and carlifeservice from svp.bin), and
     rebuilds the rebrand with the kzb-re scripts: texts, the AppConnect code changes from
     aa-carlife-shim/src/appc_patch.hpp, the tile and logo images. Each output must have the
     md5 in the lock (the files that ran in the car; keys prefixed "<base>/");
     builds the UI languages with i18n/build.py (needs docker, desay-aa:latest): our
     libUIPublic.so, which replaces the stock one, its Android Auto variant, the other
     languages' texts and the language list's flags (--accept-new renews these in the lock,
     since they change with the sheet);
  3. stages the new /svp files (svp-tree/), writes aa_version and aa_rebrand.md5;
  4. rebuilds svp.bin from the stock one with tools/svp_inject.py (stock members untouched,
     reproducible), then checks the member list against what is intended: exactly these
     additions, the two replaced files with their stock owner and mode, nothing removed, each new
     member's content, and every other member's header byte for byte as in stock;
  5. packs the container with tools/desay_pkg.py on the stock header and sidecar;
  6. verifies the result from its bytes: header changes only in the component offsets and
     lengths, app-pkg/radio/os byte-identical to stock, the sidecar's VERSION, ROLL_BACK_NO
     (1/1/1/5) and PROJECT lines equal to stock, svp members as staged, size budget;
  7. writes out/<build id>/: the container under the stock file name, its .md5, RELEASE.md, and
     svp-tree/ (for the SD-card rehearsal, start.sh option "installed").

No signature or key is involved anywhere (the container is md5-only). Run with python3 -I; the
firmware is only parsed here, never executed.
"""
import argparse
import datetime
import gzip
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)
BASES = {
    # A test unit (MQB). Against 0358 (the earlier PQ image, no longer built): libcan has one more
    # SVP_CanService method (30, GetBapColorListValue; SendCanServiceMessage moves to 31, which the
    # shim never calls); a stock androidautoservice is started that nothing on the unit calls (it
    # only switches a phone to accessory mode on its StartApp RPC), so it is left as it is;
    # appconnect is another build (appc_patch.hpp has its tables); the rebrand's images are
    # byte-identical.
    "0394": {
        "name": "5GG_035_280D_TR8257_SW0394_20200303.bin",
        "dir": os.path.join(WORK, "firmware", "MQB_5GG_035_280D", "SW0394_20200303"),
        "stock": {
            "container": "0bbae655708eb3f06e02332200549b50",
            "app-pkg.bin": "62f139472d1cff1c66559510273cdaf1",
            "radio.bin": "2042ce20afed5baabb13810a40bd8323",
            "os.bin": "66b979089e3ab1da65dcc68b02f514c8",
            "svp.bin": "12b4206746eb242138a7d3c91fd29878",
            "carlifeservice": "787b2be1675257cb713a620aef26d2ba",
            "appconnect": "51441f568e0b5e13596693e8fe0f9718",
            "VW_MP2017.kzb": "daa2ff81e531f4fa0d08211edb399287",
            "libUIPublic.so": "bf20ddc46c29235ce1d9e83cd2a9f217",
            "setting.kzb": "fb15a8ab35ebea6ffb6c560406e6eb82",
            "setting": "b43ec55a1ad674df5b2d38a7e86a4e56",
            "launcher": "b97e183c024ade5ff8887810604806aa",
        },
        "sidecar": ["VERSION:SVP30.6@C1", "ROLL_BACK_NO:1", "VERSION:MP2017_MQB_07.54_200303__C", "ROLL_BACK_NO:1",
                    "VERSION:07.54_200303__C", "ROLL_BACK_NO:1", "PROJECT:GEN2_MQB_MP2017",
                    "VERSION:MIB2-C-V20-20200303", "ROLL_BACK_NO:5"],
    },
    # The car's unit (PQ, since 2026-11-15): the 2020 image (6RD 035 187B, 2020-03-03). Against
    # 0358: the platform of the MQB 0394 (os.bin byte-identical to 0394's; libcan, libapp, libbasic,
    # libappfw, libUIPublic.so and AppConnect's resources are 0394's; the libraries the shim talks
    # through are those of both); its own builds of appconnect (appc_patch.hpp has its tables),
    # setting (the language hooks' code is 0358's, moved) and carlifeservice; androidautoservice is
    # 0394's, unused as there.
    "0317": {
        "name": "6RD_035_187B_TR7247_SW0317_20200303.bin",
        "dir": os.path.join(WORK, "firmware", "PQ_6RD_035_187B", "SW0317_20200303"),
        "stock": {
            "container": "c7d9bd20d4eaa929d91e4dcbe75bfba3",
            "app-pkg.bin": "dc7803cf7719191e063c00a9ce9524ad",
            "radio.bin": "2167757e445f9fde6d02f9b841ccb96f",
            "os.bin": "66b979089e3ab1da65dcc68b02f514c8",
            "svp.bin": "d6411ea511024f1311514b8cbc833662",
            "carlifeservice": "ecef0a2626e9f0b85a3d8d9ff766b2d4",
            "appconnect": "6e2d0e161630f6f2427714123cebf545",
            "VW_MP2017.kzb": "daa2ff81e531f4fa0d08211edb399287",
            "libUIPublic.so": "bf20ddc46c29235ce1d9e83cd2a9f217",
            "setting.kzb": "fb15a8ab35ebea6ffb6c560406e6eb82",
            "setting": "ef39e20f6e43133fbbd3a2f04044be4b",
            "launcher": "4f00f44c523ef524b1a7c5896fb46400",
        },
        "sidecar": ["VERSION:SVP30.6@C1", "ROLL_BACK_NO:1", "VERSION:MP2017_PQ_07.54_200303__C", "ROLL_BACK_NO:1",
                    "VERSION:07.54_200303__C", "ROLL_BACK_NO:1", "PROJECT:GEN2_PQ_MP2017",
                    "VERSION:MIB2-C-V20-20200303", "ROLL_BACK_NO:5"],
    },
}
APPC_PKG = "./sv.g5.mib2.appconnect-1.0.0.pkg"
SETTING_PKG = "./sv.g5.mib2.setting-1.0.0.pkg"
LAUNCHER_PKG = "./sv.g5.mib2.launcher-1.0.0.pkg"
APPC = "/storage/app/sv.g5.mib2.appconnect"
GROWTH_BUDGET = 12 * 1024 * 1024  # uncompressed bytes added to /svp

# Lock keys -> where the build finds them.
# The CarLife shim is part of aa-headless since its Rust port (`aa-headless shim`); the image's
# bin/aa-carlife-shim is a stub that execs it (svp/bin/aa-carlife-shim).
BINARIES = {
    "aa-headless": "aa-headless.armhf",
    "aa-bridge": "aa-bridge/aa-bridge.armhf",
}
REBRAND = {  # lock key: (stock key, target on the unit, svp member)
    "aa_appconnect": ("appconnect", APPC + "/bin/appconnect", "./share/aa_appconnect"),
    "aa_VW_MP2017.kzb": ("VW_MP2017.kzb", APPC + "/res/VW_MP2017.kzb", "./share/aa_VW_MP2017.kzb"),
    # The target is our libUIPublic.so (I18N), which the image has in place of the stock one.
    "aa_libUIPublic.so": ("libUIPublic.so", "/svp/lib/libUIPublic.so", "./share/aa_libUIPublic.so"),
}
# The UI languages (i18n/build.py): lock key -> svp member. They are rebuilt from the sheet, so
# --accept-new may renew them; aa_libUIPublic.so (above) is one of them.
I18N = {"libUIPublic.so": "./lib/libUIPublic.so", "aa_lang.bin": "./share/aa_lang.bin"}
with open(os.path.join(WORK, "i18n", "languages.csv"), encoding="utf-8-sig") as _f:
    for _flag in sorted(l.split(",")[3] for l in _f.read().splitlines()[1:] if l.strip()):
        I18N["aa_lang_%s.png" % _flag] = "./share/aa_lang_%s.png" % _flag
for _pic in ("theme_auto", "theme_amber", "theme_red", "theme_blue", "theme_green", "blank"):  # Settings > Appearance
    I18N["aa_%s.png" % _pic] = "./share/aa_%s.png" % _pic
REPLACED = ("./bin/carlifeservice", "./lib/libUIPublic.so")  # stock members the image replaces


def md5(data):
    return hashlib.md5(data).hexdigest()


def md5f(path):
    with open(path, "rb") as f:
        return md5(f.read())


def die(msg):
    sys.exit("build_update: " + msg)


def run(*cmd):
    print("  $", " ".join(os.path.relpath(c, WORK) if os.path.isabs(c) and c.startswith(WORK) else c for c in cmd))
    r = subprocess.run(cmd, cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in r.stdout.splitlines():
        print("    " + line)
    if r.returncode:
        die("%s failed (%d)" % (cmd[2] if cmd[0] == sys.executable else cmd[0], r.returncode))


def read_lock(path):
    lock = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                k, v = line.split()
                lock[k] = v
    return lock


def write_lock(path, lock, note):
    """Rewrites the lock with `lock`, keeping its history notes and adding `note`."""
    history = []
    if os.path.exists(path):
        history = [l.rstrip("\n") for l in open(path) if l.startswith("# ") and not l.startswith("# md5 of every")
                   and not l.startswith("# in (sysupdate")]
    with open(path, "w") as f:
        f.write("# md5 of every artifact the system update ships, and the car run each was signed off\n"
                "# in (sysupdate/README.md). build_update.py refuses anything else unless --accept-new.\n")
        for h in history + ["# " + note]:
            f.write(h + "\n")
        for k in sorted(lock):
            f.write("%s %s\n" % (k, lock[k]))


def tar_member(tar, name):
    try:
        m = tar.getmember(name)
    except KeyError:
        die("%s: not in the stock image" % name)
    return m, tar.extractfile(m).read()


def svp_meta(path):
    out = {}
    with tarfile.open(path, "r:gz") as t:
        for m in t:
            out[m.name] = (m.mode, m.uid, m.gid, m.type, m.size,
                           md5(t.extractfile(m).read()) if m.isreg() else None)
    return out


def tar_headers(path):
    """{member name: its 512-byte header} of a gzip'd tar, read raw."""
    with gzip.open(path) as g:
        raw = g.read()
    out, off = {}, 0
    while off + 512 <= len(raw):
        h = raw[off:off + 512]
        if h == b"\0" * 512:
            break
        size = int(h[124:136].rstrip(b"\0 ") or b"0", 8)
        out[h[:100].rstrip(b"\0").decode("latin1")] = h
        off += 512 + (size + 511) // 512 * 512
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", choices=sorted(BASES), default="0317", help="stock image to build from")
    ap.add_argument("--stock-dir", default=None, help="default: the base's usual folder")
    ap.add_argument("--build-id", default="aa-" + datetime.date.today().strftime("%Y%m%d"))
    ap.add_argument("--label", default="", help='a release\'s name, e.g. "Public Beta 1": in the release notes, '
                    "aa_version and Settings > System information")
    ap.add_argument("--lock", default=os.path.join(HERE, "RELEASE.lock"))
    ap.add_argument("--accept-new", action="store_true",
                    help="record the current binaries' md5s in the lock instead of refusing them")
    ap.add_argument("--out", default=None, help="default out/<build id>")
    a = ap.parse_args()
    base = BASES[a.base]
    STOCK_NAME, STOCK, SIDECAR = base["name"], base["stock"], base["sidecar"]
    prefix = a.base + "/"  # lock keys of the base's rebrand outputs (0358's, the first, had none)

    out = a.out or os.path.join(WORK, "out", a.build_id)
    tmp = out + ".work"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    lock = read_lock(a.lock)
    lock_changed = False
    new_keys = []

    print("[1] stock container")
    src = os.path.join(a.stock_dir or base["dir"], STOCK_NAME)
    if md5f(src) != STOCK["container"]:
        die("%s is not the stock %s container" % (src, a.base))
    sidecar = [l.rstrip("\n") for l in open(src + ".md5")]
    if sidecar[1:] != SIDECAR:
        die("unexpected stock sidecar: %r" % sidecar)
    parts = os.path.join(tmp, "parts")
    run(sys.executable, "-I", os.path.join(WORK, "tools/desay_pkg.py"), "unpack", src, parts)
    for p in ("app-pkg.bin", "radio.bin", "os.bin", "svp.bin"):
        if md5f(os.path.join(parts, p)) != STOCK[p]:
            die("stock %s md5 differs" % p)

    print("[2] stock inputs from the image, and the rebrand")
    stock_dir = os.path.join(tmp, "stock")
    os.makedirs(stock_dir)
    with tarfile.open(os.path.join(parts, "svp.bin"), "r:gz") as t:
        _, carlife = tar_member(t, "./bin/carlifeservice")
        _, libui = tar_member(t, "./lib/libUIPublic.so")
    with tarfile.open(os.path.join(parts, "app-pkg.bin"), "r:gz") as t:
        _, pkg = tar_member(t, APPC_PKG)
        _, spkg = tar_member(t, SETTING_PKG)
        _, lpkg = tar_member(t, LAUNCHER_PKG)
    with tarfile.open(fileobj=io.BytesIO(pkg), mode="r:gz") as t:
        _, appconnect = tar_member(t, "bin/appconnect")
        _, kzb = tar_member(t, "res/VW_MP2017.kzb")
    with tarfile.open(fileobj=io.BytesIO(spkg), mode="r:gz") as t:
        _, setting_kzb = tar_member(t, "res/VW_MP2017.kzb")
        _, setting_bin = tar_member(t, "bin/setting")
    with tarfile.open(fileobj=io.BytesIO(lpkg), mode="r:gz") as t:
        _, launcher = tar_member(t, "bin/launcher")
    for key, data in (("carlifeservice", carlife), ("libUIPublic.so", libui), ("appconnect", appconnect),
                      ("VW_MP2017.kzb", kzb), ("setting.kzb", setting_kzb), ("setting", setting_bin),
                      ("launcher", launcher)):
        if md5(data) != STOCK[key]:
            die("stock %s md5 %s, expected %s" % (key, md5(data), STOCK[key]))
        with open(os.path.join(stock_dir, key), "wb") as f:
            f.write(data)
    rb = os.path.join(tmp, "rebrand")
    run(sys.executable, "-I", os.path.join(WORK, "kzb-re/rebrand_strings.py"),
        os.path.join(stock_dir, "appconnect"), os.path.join(stock_dir, "libUIPublic.so"), rb)
    run(sys.executable, "-I", os.path.join(WORK, "kzb-re/patch_appconnect.py"),
        os.path.join(rb, "bin/appconnect"), os.path.join(rb, "aa_appconnect"))
    run(sys.executable, "-I", os.path.join(WORK, "kzb-re/rebrand_images.py"),
        os.path.join(stock_dir, "VW_MP2017.kzb"), os.path.join(rb, "aa_VW_MP2017.kzb"))
    lang = os.path.join(tmp, "i18n")
    run(sys.executable, "-I", os.path.join(WORK, "i18n/build.py"), "--stock-lib", os.path.join(stock_dir, "libUIPublic.so"),
        "--rebranded-lib", os.path.join(rb, "lib/libUIPublic.so"), "--setting-kzb", os.path.join(stock_dir, "setting.kzb"),
        "--setting-bin", os.path.join(stock_dir, "setting"), "--base", a.base,
        "--launcher-bin", os.path.join(stock_dir, "launcher"),
        "--aa-appconnect", os.path.join(rb, "aa_appconnect"), "--out", lang)

    print("[3] artifacts against the lock")
    files = {}  # artifact -> local path
    for k, rel in BINARIES.items():
        files[k] = os.path.join(WORK, rel)
    for k in REBRAND:
        files[k] = os.path.join(rb, k)
    files["aa_libUIPublic.so"] = os.path.join(lang, "aa_libUIPublic.so")
    for k in I18N:
        files[k] = os.path.join(lang, k)
    # Renewed by --accept-new: the binaries, and the outputs that change with our sources (the
    # sheet; appc_patch.hpp's code changes in aa_appconnect; rebrand_images.py's in the kzb, as
    # when its images were made whole, 2026-11-15). The other rebrand outputs must reproduce the
    # locked ones.
    renewable = set(BINARIES) | set(I18N) | {"aa_libUIPublic.so", "aa_appconnect", "aa_VW_MP2017.kzb"}
    for k, path in sorted(files.items()):
        if not os.path.isfile(path):
            die("%s missing (%s)" % (k, path))
        key = k if k in BINARIES else prefix + k
        have = md5f(path)
        if lock.get(key) == have:
            print("  %-23s %s  as locked" % (key, have))
        elif a.accept_new and (k in renewable or key not in lock):
            print("  %-23s %s  NEW (was %s): recorded in the lock, not yet run in the car" % (key, have, lock.get(key)))
            lock[key] = have
            lock_changed = True
            new_keys.append(key)
        else:
            die("%s is %s, the lock has %s%s" % (key, have, lock.get(key),
                                                 "" if k in renewable else " (the rebrand did not reproduce)"))

    # The Android Auto version Settings shows (i18n/lib/settings.cpp), from the receiver's stamp.
    stamp = files["aa-headless"] + ".version"
    try:
        aa_version, stamp_md5 = open(stamp).read().split()
    except (OSError, ValueError):
        die("%s missing or malformed: rebuild the receiver with r2_musl.sh, which writes it" % stamp)
    if stamp_md5 != md5f(files["aa-headless"]) or not re.fullmatch(r"LIVI-\d{4}-\d\d-\d\d-[0-9a-f]{7,40}", aa_version):
        die("%s does not describe aa-headless %s: rebuild the receiver with r2_musl.sh" % (stamp, md5f(files["aa-headless"])))
    print("  Android Auto version %s" % aa_version)

    print("[4] staging the /svp files")
    tree = os.path.join(tmp, "svp-tree")
    for d in ("bin", "etc", "lib", "share"):
        os.makedirs(os.path.join(tree, d))
    stage = {  # svp member -> local source
        "./bin/carlifeservice": os.path.join(HERE, "svp/bin/carlifeservice"),
        "./bin/carlifeservice.stock": os.path.join(stock_dir, "carlifeservice"),
        "./bin/aa-carlife-shim": os.path.join(HERE, "svp/bin/aa-carlife-shim"),
        "./bin/aa-headless": files["aa-headless"],
        "./bin/aa-bridge": files["aa-bridge"],
        "./etc/aa_run.sh": os.path.join(HERE, "svp/etc/aa_run.sh"),
        "./etc/aa.conf": os.path.join(HERE, "svp/etc/aa.conf"),
        "./etc/aa_android_auto_version": os.path.join(tmp, "aa_android_auto_version"),
    }
    with open(stage["./etc/aa_android_auto_version"], "w") as f:
        # Labelled: "<label> (LIVI)", which fits the row (the full stamp, 42 characters with the label,
        # was cut off); the stamp stays in aa_version, the release notes and the logs' info.txt.
        f.write(("%s (LIVI)" % a.label if a.label else aa_version) + "\n")
    for k, (_, _, member) in REBRAND.items():
        stage[member] = files[k]
    for k, member in I18N.items():
        stage[member] = files[k]
    for member, path in stage.items():
        dst = os.path.join(tree, member[2:])
        shutil.copyfile(path, dst)
        os.chmod(dst, 0o755)
    with open(os.path.join(tree, "etc/aa_rebrand.md5"), "w") as f:
        f.write("# <stock md5> <rebranded md5> <source under /svp> <target>: aa_run.sh mounts each source over\n"
                "# its target in aa mode, only while the target is the image's file and the source intact.\n")
        for k, (stock_key, target, member) in sorted(REBRAND.items()):
            want = md5f(files["libUIPublic.so"]) if k == "aa_libUIPublic.so" else STOCK[stock_key]
            f.write("%s %s %s %s\n" % (want, md5f(files[k]), member[2:], target))
    stage["./etc/aa_rebrand.md5"] = os.path.join(tree, "etc/aa_rebrand.md5")
    with open(os.path.join(tree, "etc/aa_version"), "w") as f:
        f.write("%s release%s, built from stock %s (%s)\n" % (a.build_id, " (%s)" % a.label if a.label else "", a.base,
                                                              STOCK["container"]))
        for member in sorted(stage):
            f.write("%s %s\n" % (md5f(stage[member]), member))
    stage["./etc/aa_version"] = os.path.join(tree, "etc/aa_version")
    for s in ("bin/carlifeservice", "bin/aa-carlife-shim", "etc/aa_run.sh"):
        r = subprocess.run(["sh", "-n", os.path.join(tree, s)])
        if r.returncode:
            die("sh -n %s failed" % s)

    print("[5] svp.bin")
    newparts = os.path.join(tmp, "newparts")
    os.makedirs(newparts)
    for p in ("app-pkg.bin", "radio.bin", "os.bin"):
        shutil.copyfile(os.path.join(parts, p), os.path.join(newparts, p))
    cmd = [sys.executable, "-I", os.path.join(WORK, "tools/svp_inject.py"),
           os.path.join(parts, "svp.bin"), os.path.join(newparts, "svp.bin")]
    for member in sorted(stage):
        cmd += ["--replace" if member in REPLACED else "--add", "%s=%s" % (member, stage[member])]
    run(*cmd)
    a_meta = svp_meta(os.path.join(parts, "svp.bin"))
    b_meta = svp_meta(os.path.join(newparts, "svp.bin"))
    added = sorted(set(b_meta) - set(a_meta))
    removed = sorted(set(a_meta) - set(b_meta))
    changed = sorted(n for n in set(a_meta) & set(b_meta) if a_meta[n] != b_meta[n])
    want_added = sorted(m for m in stage if m not in REPLACED)
    if added != want_added or removed or changed != sorted(REPLACED):
        die("svp.bin diff is not the intended one:\n  added %s\n  removed %s\n  changed %s" % (added, removed, changed))
    like = a_meta["./bin/connectionservice"]
    for member in stage:
        meta = b_meta[member]
        ref = a_meta.get(member, like)
        if meta[:3] != ref[:3] or meta[5] != md5f(stage[member]):
            die("%s: owner/mode %s (want %s) or content differs" % (member, meta[:3], ref[:3]))
    # Every stock member but the replaced ones keeps its exact header bytes (and, above, content).
    a_hdr, b_hdr = tar_headers(os.path.join(parts, "svp.bin")), tar_headers(os.path.join(newparts, "svp.bin"))
    moved = [n for n in a_hdr if n not in REPLACED and a_hdr[n] != b_hdr.get(n)]
    if moved:
        die("stock member headers changed: %s" % moved[:5])
    growth = sum(b_meta[n][4] for n in added) + sum(b_meta[n][4] - a_meta[n][4] for n in REPLACED)
    if growth > GROWTH_BUDGET:
        die("/svp grows by %d bytes, over the %d budget" % (growth, GROWTH_BUDGET))
    print("  members: %d stock + %d added, %d replaced; /svp grows by %d bytes" % (len(a_meta), len(added),
                                                                                     len(REPLACED), growth))

    print("[6] container")
    os.makedirs(out, exist_ok=True)
    dst = os.path.join(out, STOCK_NAME)
    run(sys.executable, "-I", os.path.join(WORK, "tools/desay_pkg.py"), "pack", "--template", src,
        "--parts", newparts, "--out", dst, "--manifest", src + ".md5")

    print("[7] verify the container from its bytes")
    with open(src, "rb") as f:
        sh = f.read(0x200000)
    with open(dst, "rb") as f:
        data = f.read()
    nh = data[:0x200000]
    diff = [i for i in range(len(sh)) if sh[i] != nh[i]]
    allowed = set()
    for slot in range(1, 5):
        base = slot * 0x200
        allowed |= set(range(base + 0x04, base + 0x08)) | set(range(base + 0x28, base + 0x2c))
    if not set(diff) <= allowed:
        die("header bytes outside the component offsets/lengths changed: %s" % [hex(i) for i in diff if i not in allowed][:8])
    check = os.path.join(tmp, "check")
    run(sys.executable, "-I", os.path.join(WORK, "tools/desay_pkg.py"), "unpack", dst, check)
    for p in ("app-pkg.bin", "radio.bin", "os.bin"):
        if md5f(os.path.join(check, p)) != STOCK[p]:
            die("%s is not byte-identical to stock" % p)
    if md5f(os.path.join(check, "svp.bin")) != md5f(os.path.join(newparts, "svp.bin")):
        die("svp.bin in the container differs from the one built")
    with gzip.open(os.path.join(check, "svp.bin")) as g:
        while g.read(1 << 20):
            pass
    side = [l.rstrip("\n") for l in open(dst + ".md5")]
    if side[0] != "%s  .//%s" % (md5(data), STOCK_NAME) or side[1:] != SIDECAR:
        die("sidecar wrong: %r" % side)
    rollback = [l for l in side if l.startswith("ROLL_BACK_NO:")]
    if rollback != ["ROLL_BACK_NO:1", "ROLL_BACK_NO:1", "ROLL_BACK_NO:1", "ROLL_BACK_NO:5"]:
        die("rollback numbers changed: %r" % rollback)
    print("  header: only offsets/lengths; app-pkg, radio, os stock; sidecar VERSION/ROLL_BACK_NO/PROJECT stock")

    print("[8] outputs")
    tree_out = os.path.join(out, "svp-tree")
    shutil.rmtree(tree_out, ignore_errors=True)
    shutil.copytree(tree, tree_out)
    if lock_changed:
        write_lock(a.lock, lock, "build %s: %s recorded by --accept-new, not yet run in the car" % (
            a.build_id, ", ".join("%s %s" % (k, lock[k][:8]) for k in sorted(new_keys))))
    container_md5 = md5(data)
    with open(os.path.join(out, "RELEASE.md"), "w") as f:
        f.write("# Android Auto system update %s%s\n\n" % (a.label + ": " if a.label else "", a.build_id))
        f.write("- Container: `%s` (%d bytes), md5 `%s`, with its `.md5` sidecar.\n" % (STOCK_NAME, len(data), container_md5))
        f.write("- Built from stock %s `%s` (%s). Only `svp.bin` differs; `app-pkg.bin`, `radio.bin` and `os.bin` are "
                "byte-identical to stock, and VERSION, ROLL_BACK_NO (1/1/1/5) and PROJECT are the stock ones.\n"
                % (a.base, STOCK["container"], [l for l in SIDECAR if l.startswith("PROJECT:")][0][8:]))
        f.write("- Android Auto: `%s` (upstream LIVI, https://github.com/f-io/LIVI).\n" % aa_version)
        f.write("- `/svp` grows by %d bytes.\n%s\n" % (growth, "- **Contains binaries not yet run in the car** (built with `--accept-new`).\n"
                                                         if lock_changed else ""))
        f.write("## svp.bin members added or replaced\n\n| Member | md5 | Size |\n|---|---|---|\n")
        for member in sorted(stage):
            f.write("| `%s`%s | `%s` | %d |\n" % (member, " (replaced)" if member in REPLACED else "",
                                               md5f(stage[member]), os.path.getsize(stage[member])))
        f.write("\nFlash, check and recovery: `sysupdate/README.md`.\n")
    shutil.rmtree(tmp)
    print("  %s\n  md5 %s" % (dst, container_md5))


if __name__ == "__main__":
    main()
