#!/usr/bin/env python3
"""Builds the language files for one base image, from its stock files and the sheet.

    python3 -I i18n/build.py --base <base> --stock-lib <stock libUIPublic.so> --rebranded-lib <rebranded one>
                             --setting-kzb <stock setting res/VW_MP2017.kzb>
                             --setting-bin <stock setting bin/setting> [--aa-appconnect <its AA build>]
                             --out <dir>

The apps' own texts are found in the base's unpacked image (i18n/fw.py, the same pinned
container); --aa-appconnect checks that every word the library rewrites in AppConnect is the same
in the Android Auto build of it.

<dir> must be inside the work tree (the compiler runs in the desay-aa:latest container, with the
work tree as /w). Writes:
  libUIPublic.so          our library with the stock texts: replaces /svp/lib/libUIPublic.so
  aa_libUIPublic.so       the same with the Android Auto texts (rebrand_strings.py's library):
                          bind-mounted over the first in aa mode, as the rebrand was
  aa_lang.bin             the other languages' texts (build_table.py)
  aa_lang_<flag>.png      the language list's flags (flags.py)
  aa_theme_<key>.png, aa_blank.png  Settings > Appearance's pictures (flags.py --appearance)
(All of them go flat into /svp/share, a folder the image already has.)
The same inputs give the same bytes (checked against sysupdate/RELEASE.lock by the caller).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)


def run(*cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in r.stdout.splitlines():
        print("    " + line)
    if r.returncode:
        sys.exit("i18n/build.py: %s failed (%d)" % (os.path.basename(cmd[2] if cmd[0] == sys.executable else cmd[0]),
                                                    r.returncode))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--stock-lib", required=True)
    ap.add_argument("--rebranded-lib", required=True)
    ap.add_argument("--setting-kzb", required=True)
    ap.add_argument("--setting-bin", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--aa-appconnect")
    ap.add_argument("--launcher-bin", help="the stock bin/launcher (Settings > Appearance's launcher hooks)")
    ap.add_argument("--radio-bin", help="the stock bin/radio (station names on the FM presets)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.commonpath([out, WORK]) != WORK:
        sys.exit("i18n/build.py: --out must be inside %s" % WORK)
    work = os.path.join(out, "work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    py = [sys.executable, "-I"]
    run(*py, os.path.join(HERE, "gen_table.py"), a.stock_lib, a.stock_lib, os.path.join(work, "plain.inc"))
    run(*py, os.path.join(HERE, "gen_table.py"), a.rebranded_lib, a.stock_lib, os.path.join(work, "aa.inc"))
    run(*py, os.path.join(HERE, "gen_settings.py"), a.setting_bin, os.path.join(work, "settings.inc"))
    run(*py, os.path.join(HERE, "literals.py"), "table", a.base, os.path.join(work, "literals.inc"))
    run(*py, os.path.join(HERE, "gen_texts.py"), os.path.join(work, "texts.inc"))
    if a.launcher_bin:
        run(*py, os.path.join(HERE, "gen_launcher.py"), a.launcher_bin, os.path.join(work, "launcher.inc"))
    if a.radio_bin:
        run(*py, os.path.join(HERE, "gen_radio.py"), a.radio_bin, os.path.join(work, "radio.inc"))
    if a.aa_appconnect:
        run(*py, os.path.join(HERE, "literals.py"), "verify", os.path.join(work, "literals.inc"), "appconnect", a.aa_appconnect)
    w = lambda p: "/w/" + os.path.relpath(p, WORK)  # noqa: E731
    settings = "-DAA_SETTINGS='\"%s\"' -DAA_LITERALS='\"%s\"' -DAA_TEXTS='\"%s\"'" % (
        w(os.path.join(work, "settings.inc")), w(os.path.join(work, "literals.inc")), w(os.path.join(work, "texts.inc")))
    if a.launcher_bin:
        settings += " -DAA_LAUNCHER='\"%s\"'" % w(os.path.join(work, "launcher.inc"))
    if a.radio_bin:
        settings += " -DAA_RADIO='\"%s\"'" % w(os.path.join(work, "radio.inc"))
    script = ("set -e; bash /w/i18n/lib/build.sh %s %s plain %s; bash /w/i18n/lib/build.sh %s %s aa %s"
              % (w(os.path.join(work, "plain.inc")), w(os.path.join(out, "libUIPublic.so")), settings,
                 w(os.path.join(work, "aa.inc")), w(os.path.join(out, "aa_libUIPublic.so")), settings))
    run("docker", "run", "--rm", "--platform", "linux/arm64", "-v", WORK + ":/w", "desay-aa:latest", "bash", "-c", script)
    run(*py, os.path.join(HERE, "build_table.py"), a.stock_lib, os.path.join(out, "aa_lang.bin"))
    run(*py, os.path.join(HERE, "flags.py"), a.setting_kzb, os.path.join(work, "flags"), "--appearance",
        os.path.join(work, "appearance"))
    for name in sorted(os.listdir(os.path.join(work, "flags"))):
        shutil.copyfile(os.path.join(work, "flags", name), os.path.join(out, "aa_lang_" + name))
    for name in sorted(os.listdir(os.path.join(work, "appearance"))):
        shutil.copyfile(os.path.join(work, "appearance", name), os.path.join(out, "aa_" + name))
    shutil.rmtree(work)


if __name__ == "__main__":
    main()
