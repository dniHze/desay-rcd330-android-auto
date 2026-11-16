#!/usr/bin/env python3
"""Tests our libUIPublic.so on the unit's own loader and libraries (qemu-arm -L .scratch/sysroot).

    python3 -I i18n/test_lib.py          (docker: desay-aa:latest to build, debian:bookworm + qemu to run)

For each base (0317, 0394) and the Android Auto variant of 0317, a test build of the library
(the same source, its state and data files under /aa-lang-test, its language functions
visible) is loaded by i18n/test/test_lib.cpp, which plays the app's side. Checked against the
stock phrase book and the sheet:
  off      the state "off": every lookup, both car codes, and every row as the stock library
  missing  no state: it is created as "en", English with either car code, and the car is asked
           for English once, only when its code was Chinese
  zh, uk   Chinese, or the sheet's Ukrainian (English where empty), in both columns
  live     a state written in place by another process applies at the next lookup
  corrupt  a damaged state gives English; a short one is not mapped (no fault)
  baddata  strings.bin for another phrase book (its English md5 differs): English and Chinese only,
           extras refused
  select   aa_lang_select writes the record and applies it
  aa       the Android Auto variant's texts rename CarLife in every language
  tramp    the Settings hooks' trampoline (moving a leading literal load) and entry jump
  bt       the Bluetooth app's pop-up hook: held back only while the shim's file says Android Auto
           is on screen and the shim runs; refused on code that is not AddUserID's
  lit      an own text read as an app reads it (ldr + add pc) follows the language: the sheet's
           text in uk, de and tr, the stock text in en and zh; a site whose word is not the
           stock one is refused; in the Android Auto variant, CarLife reads Android Auto
"""
import csv
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_table  # noqa: E402
import extract  # noqa: E402
import fw  # noqa: E402
import literals  # noqa: E402

T = os.path.join(fw.OUT, "test")
N = 1653  # phrases in both bases' book
ROOT = "/aa-lang-test"
STATE = ROOT + "/state/aa/lang"
DATA = ROOT + "/strings.bin"
CHECKS = []


def w(path):
    return "/w/" + os.path.relpath(path, fw.WORK)


def docker(image, script, platform="linux/arm64"):
    r = subprocess.run(["docker", "run", "--rm", "--platform", platform, "-v", fw.WORK + ":/w", image, "bash", "-c", script],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        print(r.stdout)
        sys.exit("docker %s failed" % image)
    return r.stdout


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def check(name, ok, detail=""):
    CHECKS.append((name, ok))
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else ": " + detail))


def read_dump(path):
    out = {}
    with open(path, encoding="utf-8", newline="\n") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            out[int(p[0])] = p[1:]
    return out


def compare(name, dump, want):
    bad = [i for i in want if dump.get(i) != want[i]]
    check(name, not bad, "%d of %d differ, e.g. %s: %r want %r" % (len(bad), len(want), bad[0], dump.get(bad[0]), want[bad[0]])
          if bad else "")


def main():
    shutil.rmtree(T, ignore_errors=True)
    os.makedirs(T)
    with open(extract.SHEET, encoding="utf-8-sig", newline="") as f:
        sheet = {r["id"]: r for r in csv.DictReader(f)}
    # Inputs: tables, data files, the AA variant's rebranded library.
    targets = []
    for base in fw.BASE_NAMES:
        lib = os.path.join(fw.unpack(base), "svp", "lib", "libUIPublic.so")
        targets.append((base, "plain", lib, lib))
    rb = os.path.join(T, "rebrand")
    tree = fw.tree("0317")
    subprocess.run([sys.executable, "-I", os.path.join(fw.WORK, "kzb-re", "rebrand_strings.py"),
                    os.path.join(tree, "apps", "sv.g5.mib2.appconnect", "bin", "appconnect"),
                    os.path.join(tree, "svp", "lib", "libUIPublic.so"), rb], check=True, stdout=subprocess.DEVNULL)
    targets.append(("0317", "aa", os.path.join(rb, "lib", "libUIPublic.so"), os.path.join(tree, "svp", "lib", "libUIPublic.so")))
    cmds = []
    for base, variant, lib, plain in targets:
        tag = "%s-%s" % (base, variant)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_table.py"), lib, plain, os.path.join(T, tag + ".inc")],
                       check=True, stdout=subprocess.DEVNULL)
        lits = os.path.join(T, "literals-%s.inc" % base)
        if not os.path.exists(lits):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "literals.py"), "table", base, lits],
                           check=True, stdout=subprocess.DEVNULL)
        cmds.append("bash /w/i18n/lib/build.sh %s %s %s -DAA_LANG_TEST -DAA_LANG_STATE='\"%s\"' -DAA_LANG_DATA='\"%s\"' "
                    "-DAA_LITERALS='\"%s\"' >/dev/null"
                    % (w(os.path.join(T, tag + ".inc")), w(os.path.join(T, tag + ".so")), variant, STATE, DATA, w(lits)))
    for base in fw.BASE_NAMES:
        build_table.build(os.path.join(fw.tree(base), "svp", "lib", "libUIPublic.so"), os.path.join(T, "strings-%s.bin" % base))
    # Both bases have the same phrase book; a strings.bin for another one differs in the md5 of
    # its English texts (the header's, at byte 20).
    data = bytearray(open(os.path.join(T, "strings-0317.bin"), "rb").read())
    data[20] = ord("1") if data[20] == ord("0") else ord("0")
    open(os.path.join(T, "strings-other.bin"), "wb").write(data)
    xt = "/w/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf-g++"
    cmds.append("%s -std=gnu++11 -O1 -rdynamic -DAA_LANG_STATE='\"%s\"' -o %s /w/i18n/test/test_lib.cpp -ldl"
                % (xt, STATE, w(os.path.join(T, "test_lib"))))
    docker("desay-aa:latest", "set -e; " + "; ".join(cmds))
    print("built the test libraries and the harness")

    # The scenarios, all in one qemu container.
    run = ["set -e", "apt-get update -qq >/dev/null && apt-get install -y -qq qemu-user >/dev/null",
           "Q='qemu-arm -L /w/.scratch/sysroot'", "H=%s" % w(os.path.join(T, "test_lib"))]
    out = w(T)

    def scenario(tag, name, data, state, args):
        lines = ["rm -rf %s && mkdir -p %s/state/aa" % (ROOT, ROOT)]
        if data == "good":
            lines.append("cp %s/strings-%s.bin %s" % (out, tag.split("-")[0], DATA))
        elif data == "other":
            lines.append("cp %s/strings-other.bin %s" % (out, DATA))
        if state is not None:  # a code, or a raw record ("raw:...")
            fmt = state[4:] if state.startswith("raw:") else "AALANG1 %-23s\\n"
            lines.append("printf '%s' %s > %s" % (fmt, "" if state.startswith("raw:") else state, STATE))
        lines.append("$Q $H %s/%s.so n=%d %s > %s/%s-%s.log 2>&1" % (out, tag, N, " ".join(args), out, tag, name))
        lines.append("cp %s %s/%s-%s.state 2>/dev/null || true" % (STATE, out, tag, name))
        run.extend(lines)

    rec = lambda code: "AALANG1 %-23s\n" % code  # noqa: E731
    for tag in ("0317-plain", "0394-plain"):
        d = "%s/%s-" % (out, tag)
        scenario(tag, "off", "good", "off", ["car=0", "dump=%soff0.tsv" % d, "car=1", "dump=%soff1.tsv" % d])
        scenario(tag, "missing1", "good", None, ["car=1", "dump=%smissing1.tsv" % d, "info"])
        scenario(tag, "missing0", "good", None, ["car=0", "dump=%smissing0.tsv" % d])
        scenario(tag, "zh", "good", "zh", ["car=0", "dump=%szh.tsv" % d])
        scenario(tag, "uk", "good", "uk", ["car=1", "dump=%suk.tsv" % d, "info"])
        scenario(tag, "live", "good", "en",
                 ["car=0", "dump=%slive-en.tsv" % d, "write=de", "dump=%slive-de.tsv" % d, "write=zh", "dump=%slive-zh.tsv" % d])
        scenario(tag, "corrupt", "good", "raw:AALANG1 xx#garbage..............\\n", ["car=0", "dump=%scorrupt.tsv" % d])
        scenario(tag, "short", "good", "raw:AAL", ["car=0", "dump=%sshort.tsv" % d])
        scenario(tag, "baddata", "other", "uk", ["car=0", "dump=%sbaddata.tsv" % d, "info"])
        scenario(tag, "select", "good", "en", ["select=tr", "car=0", "dump=%sselect.tsv" % d, "select=xx"])
        # The software update app: stock whatever the state (the harness run from its path).
        scenario(tag, "update", "good", "uk", ["car=0", "dump=%supdate.tsv" % d])
        k = max(j for j, line in enumerate(run) if line.startswith("$Q $H "))
        run[k] = run[k].replace("$Q $H ", "mkdir -p %s/sv.g5.mib2.update/bin && cp $H %s/sv.g5.mib2.update/bin/update && "
                                "$Q %s/sv.g5.mib2.update/bin/update " % (ROOT, ROOT, ROOT))
    scenario("0317-aa", "uk", "good", "uk", ["car=0", "dump=%s/0317-aa-uk.tsv" % out])
    scenario("0317-plain", "tramp", "good", "en", ["tramp"])
    scenario("0317-plain", "bt", "good", "en", ["bt=%s/ontop" % ROOT])
    lit_rows = [r for r in sheet.values() if r["id"].startswith("lit:")]
    ents = literals.entries(lit_rows)
    welcome = next(k for k, (rid, j, _) in enumerate(ents) if rid == "lit:appconnect:0")
    line2 = next(k for k, (rid, j, _) in enumerate(ents) if rid == "lit:appconnect:3" and j == 1)
    carlife = next(k for k, (rid, j, _) in enumerate(ents) if rid == "lit:appconnect:32" and j < 0)
    seq = ["littext", "write=de", "littext", "write=zh", "littext", "write=en", "littext", "write=tr", "littext"]
    scenario("0317-plain", "lit", "good", "uk", ["lit=%d" % welcome] + seq)
    scenario("0317-plain", "litline", "good", "uk", ["lit=%d" % line2, "littext"])
    scenario("0317-plain", "litbad", "good", "uk", ["lit=%d,bad" % welcome, "littext"])
    lenseq = [x.replace("littext", "litlentext") for x in seq]
    scenario("0317-plain", "litlen", "good", "uk", ["lit=%d,len" % welcome] + lenseq)
    scenario("0317-plain", "litlenbad", "good", "uk", ["lit=%d,len,badlen" % welcome, "litlentext"])
    scenario("0317-aa", "lit", "good", "uk", ["lit=%d" % carlife, "littext"])
    docker("debian:bookworm", "\n".join(run))
    print("ran the scenarios under qemu")

    for base in fw.BASE_NAMES:
        tag = base + "-plain"
        p = lambda name: os.path.join(T, "%s-%s" % (tag, name))  # noqa: E731
        book = fw.phrasebook(os.path.join(fw.tree(base), "svp", "lib", "libUIPublic.so"))
        stock_zh = {i: z.decode() for i, z, _ in book}
        stock_en = {i: e.decode() for i, _, e in book}
        over = build_table.overrides()
        zh = {i: over.get("zh", {}).get(i, t) for i, t in stock_zh.items()}
        en = {i: over.get("en", {}).get(i, t) for i, t in stock_en.items()}

        def lang(code):
            return {i: (sheet[str(i)][code].replace("\\n", "\n") or en[i]) for i in en}

        def all4(m):
            return {i: [esc(m[i])] * 4 for i in m}
        compare("%s off, car 0: stock English, rows stock" % base, read_dump(p("off0.tsv")),
                {i: [esc(stock_en[i]), esc(stock_en[i]), esc(stock_zh[i]), esc(stock_en[i])] for i in en})
        compare("%s off, car 1: stock Chinese, rows stock" % base, read_dump(p("off1.tsv")),
                {i: [esc(stock_zh[i]), esc(stock_zh[i]), esc(stock_zh[i]), esc(stock_en[i])] for i in en})
        log1 = open(p("missing1.log")).read()
        compare("%s no state, car 1: English" % base, read_dump(p("missing1.tsv")), all4(en))
        check("%s no state: created as en" % base, open(p("missing1.state")).read() == rec("en"))
        check("%s no state, car 1: the car asked for English once" % base, log1.count("CHANGE 0") == 1, log1)
        log0 = open(p("missing0.log")).read()
        compare("%s no state, car 0: English" % base, read_dump(p("missing0.tsv")), all4(en))
        check("%s no state, car 0: the car left alone" % base, "CHANGE" not in log0, log0)
        compare("%s zh: Chinese in both columns" % base, read_dump(p("zh.tsv")), all4(zh))
        compare("%s uk, car 1: Ukrainian in both columns" % base, read_dump(p("uk.tsv")), all4(lang("uk")))
        info = open(p("uk.log")).read()
        order = sorted(extract.languages(), key=lambda l: int(l["order"]))
        want = " ".join("%s(%s|/svp/share/aa_lang_%s.png)" % (l["code"], l["endonym"], l["flag"]) for l in order)
        check("%s languages in the list order (by name)" % base, "current uk; languages %s\n" % want in info, info)
        compare("%s live: en" % base, read_dump(p("live-en.tsv")), all4(en))
        compare("%s live: de written by another process" % base, read_dump(p("live-de.tsv")), all4(lang("de")))
        compare("%s live: then zh" % base, read_dump(p("live-zh.tsv")), all4(zh))
        compare("%s damaged state: English" % base, read_dump(p("corrupt.tsv")), all4(en))
        compare("%s short state: English, no fault" % base, read_dump(p("short.tsv")), all4(en))
        info = open(p("baddata.log")).read()
        compare("%s strings.bin of another phrase book: stock English" % base, read_dump(p("baddata.tsv")), all4(stock_en))
        check("%s strings.bin of another phrase book: only en and zh offered" % base,
              "languages en(English (US)|-) zh(中文|-)" in info, info)
        compare("%s the update app: stock with the state uk" % base, read_dump(p("update.tsv")),
                {i: [esc(stock_en[i]), esc(stock_en[i]), esc(stock_zh[i]), esc(stock_en[i])] for i in en})
        check("%s the Language row: English \"Language\", Chinese \"语言 / Language\"" % base,
              read_dump(p("missing0.tsv"))[1423][0] == "Language" and read_dump(p("zh.tsv"))[1423][0] == "语言 / Language")
        log = open(p("select.log")).read()
        compare("%s select tr" % base, read_dump(p("select.tsv")), all4(lang("tr")))
        check("%s select: record written, unknown code refused" % base,
              open(p("select.state")).read() == rec("tr") and "select tr: 0" in log and "select xx: -1" in log, log)
    dump = read_dump(os.path.join(T, "0317-aa-uk.tsv"))
    named = [i for i in range(N) if "CarLife" in sheet[str(i)]["uk"]]
    check("0317 aa: CarLife renamed in Ukrainian (%d phrases)" % len(named),
          all("CarLife" not in dump[i][0] and "Android Auto" in dump[i][0] for i in named),
          str([dump[i][0] for i in named if "CarLife" in dump[i][0]][:3]))
    log = open(os.path.join(T, "0317-plain-bt.log")).read()
    check("bluetooth pop-ups: held back only while Android Auto is on screen and the shim runs",
          "bt 1007 0 1007 1007 1007\n" in log, log)
    check("bluetooth pop-ups: no hook on code that is not AddUserID's", "bt refused" in log, log)
    log = open(os.path.join(T, "0317-plain-tramp.log")).read()
    check("trampoline: a leading literal load moved, then the entry hooked",
          "tramp %d hooked %d" % (5 + 0x12345, 6 + 0x12345) in log, log)
    own = {rid: r for rid, r in sheet.items() if rid.startswith("lit:")}
    tr = lambda rid, code, j=-1: (own[rid][code].split("\\n")[j] if j >= 0 else own[rid][code]).replace("\\n", "\n")  # noqa: E731
    log = open(os.path.join(T, "0317-plain-lit.log")).read()
    got = [l[len("littext "):] for l in log.splitlines() if l.startswith("littext ")]
    want = [esc(tr("lit:appconnect:0", c)) if c else "stock own text" for c in ("uk", "de", None, None, "tr")]
    check("own text: uk, de, zh (stock), en (stock), tr", "lit taken" in log and got == want, "%r want %r" % (got, want))
    log = open(os.path.join(T, "0317-plain-litlen.log")).read()
    got = [l[len("littext "):] for l in log.splitlines() if l.startswith("littext ")]
    check("own text with its length (a pop-up's): whole in uk, de, tr; the stock length back for zh, en",
          "lit taken" in log and got == want, "%r want %r" % (got, want))
    log = open(os.path.join(T, "0317-plain-litlenbad.log")).read()
    check("own text with its length: a length instruction that is not stock is refused",
          "lit refused" in log and "littext stock own text" in log, log)
    log = open(os.path.join(T, "0317-plain-litline.log")).read()
    check("own text: a line of a message", "littext %s\n" % esc(tr("lit:appconnect:3", "uk", 1)) in log, log)
    log = open(os.path.join(T, "0317-plain-litbad.log")).read()
    check("own text: a site that is not stock is refused", "lit refused" in log and "littext stock own text" in log, log)
    log = open(os.path.join(T, "0317-aa-lit.log")).read()
    t = tr("lit:appconnect:32", "uk").replace("Baidu CarLife", "Android Auto")
    check("own text, Android Auto variant: CarLife renamed", "littext %s\n" % esc(t.replace("\\n", "\n")) in log and
          "CarLife" not in log, log)
    bad = sum(1 for _, ok in CHECKS if not ok)
    print("%d checks, %d failed" % (len(CHECKS), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
